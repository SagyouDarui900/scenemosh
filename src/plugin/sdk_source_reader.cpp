// sdk_source_reader.cpp — see sdk_source_reader.hpp.

#include "sdk_source_reader.hpp"

#include <windows.h>

#include <chrono>
#include <cstring>

#include "frame_fit.hpp"

namespace hybrid::plugin {

namespace {

// Fill an RGBA buffer with opaque black.
void fill_black(std::span<std::uint8_t> rgba) {
    for (std::size_t i = 0; i + 4 <= rgba.size(); i += 4) {
        rgba[i + 0] = 0;
        rgba[i + 1] = 0;
        rgba[i + 2] = 0;
        rgba[i + 3] = 255;
    }
}

// Layer rendering may involve heavy effects on the referenced object,
// so it gets a longer budget than screen rendering.
constexpr std::chrono::seconds kLayerTimeout{30};
// Design doc section 14: one screen frame measured 27-110ms; 5s is the
// documented timeout for screen capture.
constexpr std::chrono::seconds kScreenTimeout{5};

} // namespace

void on_rendering_done(void* param, int /*frame*/, const void* buffer,
                       int width, int height, int pitch) {
    if (param == nullptr) return;
    // Take ownership of the holder; the shared state itself stays alive
    // through the waiter's copy even if we run after its timeout.
    const std::unique_ptr<std::shared_ptr<RenderRequest>> holder(
        static_cast<std::shared_ptr<RenderRequest>*>(param));
    const std::shared_ptr<RenderRequest> state = holder ? *holder : nullptr;
    if (!state) return;
    if (state->callback_fired.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->done = true;
        state->ok = buffer != nullptr && width > 0 && height > 0;
        state->width = width;
        state->height = height;
        if (state->ok) {
            const std::size_t row_bytes =
                static_cast<std::size_t>(width) * 4;
            state->rgba.resize(row_bytes *
                               static_cast<std::size_t>(height));
            const auto* src = static_cast<const std::uint8_t*>(buffer);
            for (int y = 0; y < height; ++y) {
                std::memcpy(state->rgba.data() +
                                static_cast<std::size_t>(y) * row_bytes,
                            src + static_cast<std::size_t>(y) *
                                      static_cast<std::size_t>(pitch),
                            row_bytes);
            }
        }
    }
    state->cv.notify_all();
}

namespace {

// Resolve the object on a layer at a scene frame, inside a read
// section. The handle is returned to the caller; it stays valid while
// the project is unchanged (assumption, see the header note).
struct ResolveParam {
    int layer = 0;
    int frame = 0;
    OBJECT_HANDLE object = nullptr;
};

void resolve_object(void* param, EDIT_SECTION* edit) {
    auto* p = static_cast<ResolveParam*>(param);
    p->object = edit->find_object(p->layer, p->frame);
    if (p->object == nullptr) return;
    const OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(p->object);
    // find_object returns the first object at or after the frame; make
    // sure it actually covers the frame we asked for.
    if (p->frame < lf.start || p->frame > lf.end) p->object = nullptr;
}

// Wait for one render request and report the size the host actually
// rendered. No size is assumed here: rendering_object_video returns
// the object's own rendered image, whose size is the object's size,
// not necessarily the scene size (design doc section 19).
bool wait_for_render(const std::shared_ptr<RenderRequest>& state,
                     std::chrono::seconds timeout, ILogger* logger,
                     const char* what, int& width, int& height) {
    std::unique_lock<std::mutex> lock(state->mutex);
    const bool finished = state->cv.wait_for(lock, timeout, [&] {
        return state->done;
    });
    if (!finished) {
        log_error(logger, std::string(what) + ": rendering timed out");
        return false;
    }
    if (!state->ok) {
        log_error(logger, std::string(what) + ": rendering failed");
        return false;
    }
    width = state->width;
    height = state->height;
    return true;
}

bool wait_request(const std::shared_ptr<RenderRequest>& state,
                  std::chrono::seconds timeout, std::span<std::uint8_t> rgba,
                  int width, int height, ILogger* logger,
                  const char* what) {
    int rendered_w = 0;
    int rendered_h = 0;
    if (!wait_for_render(state, timeout, logger, what, rendered_w,
                         rendered_h)) {
        return false;
    }
    if (rendered_w != width || rendered_h != height) {
        // Both sizes are logged: the mismatch is usually a clip
        // whose resolution differs from the scene, and the numbers
        // tell which side is which.
        log_error(logger, std::string(what) + ": rendered size " +
                              std::to_string(rendered_w) + "x" +
                              std::to_string(rendered_h) +
                              " does not match the expected " +
                              std::to_string(width) + "x" +
                              std::to_string(height));
        return false;
    }
    if (rgba.size() <
        static_cast<std::size_t>(width) * height * 4) {
        log_error(logger, std::string(what) + ": output buffer too small");
        return false;
    }
    std::memcpy(rgba.data(), state->rgba.data(), state->rgba.size());
    return true;
}

// Issue one rendering_object_video request for the object. On success
// the state is filled by the callback (which owns the holder); on
// rejection the holder is deleted here.
bool request_layer_render(EDIT_HANDLE* edit, OBJECT_HANDLE object,
                          int scene_frame,
                          std::shared_ptr<RenderRequest>& state_out,
                          ILogger* logger) {
    auto state = std::make_shared<RenderRequest>();
    auto* holder = new std::shared_ptr<RenderRequest>(state);
    const bool requested = edit->rendering_object_video(
        object, scene_frame, /*apply_effect=*/true, holder,
        &on_rendering_done);
    if (!requested) {
        delete holder;
        log_error(logger, "layer render: rendering request rejected");
        return false;
    }
    state_out = std::move(state);
    return true;
}

} // namespace

LayerSourceReader::LayerSourceReader(EDIT_HANDLE* edit, int layer,
                                     std::int64_t base_frame,
                                     std::int64_t frame_count, int width,
                                     int height, ILogger* logger)
    : edit_(edit),
      layer_(layer),
      base_frame_(base_frame),
      frame_count_(frame_count),
      width_(width),
      height_(height),
      logger_(logger) {}

bool LayerSourceReader::read_frame(std::int64_t index,
                                   std::span<std::uint8_t> rgba) {
    if (edit_ == nullptr || index < 0 || index >= frame_count_) return false;
    const int scene_frame =
        static_cast<int>(base_frame_ + index); // frames fit in int here
    if (rgba.size() <
        static_cast<std::size_t>(width_) * height_ * 4) {
        log_error(logger_, "layer render: output buffer too small");
        return false;
    }

    ResolveParam param{layer_, scene_frame, nullptr};
    if (!edit_->call_read_section_param(&param, &resolve_object)) {
        log_error(logger_, "layer render: could not lock the edit section");
        return false;
    }
    if (param.object == nullptr) {
        // Gap on the layer (a cut-centred segment reaches past the
        // start of a short object before the cut): a black frame
        // keeps the encode going instead of failing the whole job.
        // Logged once; the frame itself is silent after that.
        if (!gap_logged_) {
            gap_logged_ = true;
            log_warning(logger_, "layer render: no object on the layer at "
                                 "frame " +
                                     std::to_string(scene_frame) +
                                     "; using a black frame");
        }
        fill_black(rgba);
        return true;
    }

    std::shared_ptr<RenderRequest> state;
    if (!request_layer_render(edit_, param.object, scene_frame, state,
                              logger_)) {
        return false;
    }
    int rendered_w = 0;
    int rendered_h = 0;
    if (!wait_for_render(state, kLayerTimeout, logger_, "layer render",
                         rendered_w, rendered_h)) {
        return false;
    }
    if (rendered_w == width_ && rendered_h == height_) {
        std::memcpy(rgba.data(), state->rgba.data(), state->rgba.size());
        return true;
    }
    // The object rendered at its own size, not the scene size:
    // scale it to the scene size with bilinear interpolation
    // (aspect-fit with black padding for aspect mismatches). Logged once
    // per distinct rendered size, with the size the job ends up with.
    if (logged_sizes_.insert({rendered_w, rendered_h}).second) {
        log_info(logger_, "layer render: frame is " +
                              std::to_string(rendered_w) + "x" +
                              std::to_string(rendered_h) +
                              ", scaling to scene size " +
                              std::to_string(width_) + "x" +
                              std::to_string(height_));
    }
    fit_frame_bilinear(state->rgba, rendered_w, rendered_h, rgba, width_,
                       height_);
    return true;
}

bool LayerSourceReader::probe_frame_size(int& /*width*/,
                                         int& /*height*/) {
    // No probe: the scene size is the job size and every frame is
    // fitted to it in read_frame.
    return false;
}

ScreenSourceReader::ScreenSourceReader(EDIT_HANDLE* edit,
                                       std::int64_t base_frame,
                                       std::int64_t frame_count, int width,
                                       int height, ILogger* logger)
    : edit_(edit),
      base_frame_(base_frame),
      frame_count_(frame_count),
      width_(width),
      height_(height),
      logger_(logger) {}

bool ScreenSourceReader::read_frame(std::int64_t index,
                                    std::span<std::uint8_t> rgba) {
    if (edit_ == nullptr || index < 0 || index >= frame_count_) return false;
    const int scene_frame = static_cast<int>(base_frame_ + index);

    auto state = std::make_shared<RenderRequest>();
    auto* holder = new std::shared_ptr<RenderRequest>(state);
    const bool requested = edit_->rendering_scene_video(
        scene_frame, holder, &on_rendering_done);
    if (!requested) {
        delete holder;
        log_error(logger_, "screen render: rendering request rejected");
        return false;
    }
    return wait_request(state, kScreenTimeout, rgba, width_, height_, logger_,
                        "screen render");
}

namespace {

struct FramebufferScan {
    int layer = 0;
    int start_frame = 0;
    int end_frame = 0;
    bool found = false;
};

void scan_framebuffer(void* param, EDIT_SECTION* edit) {
    auto* scan = static_cast<FramebufferScan*>(param);
    int frame = scan->start_frame;
    while (frame <= scan->end_frame && !scan->found) {
        OBJECT_HANDLE object = edit->find_object(scan->layer, frame);
        if (object == nullptr) break;
        const OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);
        const int count = edit->get_effect_list(object, nullptr, 0);
        if (count > 0) {
            std::vector<EFFECT_HANDLE> effects(
                static_cast<std::size_t>(count));
            const int got = edit->get_effect_list(
                object, effects.data(), count);
            for (int i = 0; i < got; ++i) {
                const LPCWSTR name = edit->get_effect_name(effects[i]);
                if (name != nullptr &&
                    std::wstring(name) == L"フレームバッファ") {
                    scan->found = true;
                    break;
                }
            }
        }
        frame = lf.end + 1;
    }
}

} // namespace

bool layer_has_framebuffer(EDIT_SECTION* edit, int layer, int start_frame,
                           int end_frame) {
    // This helper is called from a button callback, which receives an
    // EDIT_SECTION* directly; it does not take the read lock itself.
    FramebufferScan scan{layer, start_frame, end_frame, false};
    scan_framebuffer(&scan, edit);
    return scan.found;
}

} // namespace hybrid::plugin
