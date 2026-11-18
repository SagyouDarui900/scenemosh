// render.cpp — func_proc_video for the SceneMosh media object.
//
// Per frame, in order:
//   1. Make sure the instance has a uid (FILTER_ITEM_DATA, written
//      in place; see items.hpp).
//   2. Passthrough mode: copy the referenced layer's image into this
//      object and return.
//   3. Look for the newest finished glitch cache file (generations
//      0..7) for the current settings. When found, fetch the frame
//      through CACHE_HANDLE::get_video_file_cache and set it as this
//      object's image.
//   4. Otherwise, when enabled, draw the "not generated" marker.
//      When disabled, leave the object empty.
//
// Deviation from design doc section 4: the cache frame is applied
// with FILTER_PROC_VIDEO::set_image_data, not
// set_image_resource_data("object", ...). set_image_data is the
// operation the SDK sample uses to replace an object's image, and
// its contract is explicit in filter2.h; whether writing the
// "object" resource also updates the displayed image is not
// documented. If set_image_data turns out to be wrong on a real
// machine, switching to set_image_resource_data is a one-line change
// in apply_cache_frame().
//
// The host may call this from its render thread, not the UI thread;
// no UI is touched here, only the cache handle and the log.

#include <windows.h>

#include <cstring>
#include <memory>
#include <vector>

#include "sdk.hpp"

#include "cache_index.hpp"
#include "context.hpp"
#include "frame_fit.hpp"
#include "hash.hpp"
#include "items.hpp"
#include "marker_cache.hpp"
#include "sdk_logger.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace hybrid::plugin {
namespace {

// Dark checkerboard with a magenta border, generated per frame size.
// No text is drawn: drawing text would need a font and a rasteriser.
std::vector<PIXEL_RGBA> make_marker_buffer(int width, int height) {
    std::vector<PIXEL_RGBA> buffer(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool checker = ((x / 16) + (y / 16)) % 2 == 0;
            PIXEL_RGBA px{};
            if (x < 4 || y < 4 || x >= width - 4 || y >= height - 4) {
                px.r = 255; px.g = 0; px.b = 255; px.a = 255; // magenta
            } else if (checker) {
                px.r = 48; px.g = 48; px.b = 48; px.a = 255;
            } else {
                px.r = 24; px.g = 24; px.b = 24; px.a = 255;
            }
            buffer[static_cast<std::size_t>(y) * width + x] = px;
        }
    }
    return buffer;
}

// One marker buffer per size, reused across preview frames (F2).
// The buffer was rebuilt on every frame (~5 ms at 1920x1080)
// although the picture is the same while the size is unchanged.
MarkerCache<PIXEL_RGBA>& marker_cache() {
    static MarkerCache<PIXEL_RGBA> cache;
    return cache;
}

// Snapshot the current item values. The interval comes from the
// object itself (scene-relative frames); fps and size from the scene.
Settings render_settings(const SCENE_INFO& scene, const OBJECT_INFO& object,
                         const std::string& uid,
                         const std::filesystem::path& cache_dir) {
    Settings s;
    s.uid = uid;
    s.ref_mode = ref_mode_item.value == 1 ? RefMode::Screen : RefMode::Layer;
    s.ref_layer = static_cast<int>(ref_layer_item.value) - 1;
    s.frame_offset = static_cast<std::int64_t>(frame_offset_item.value);
    s.preset = static_cast<PresetId>(preset_item.value);
    s.strength = static_cast<int>(strength_item.value);
    s.seed = static_cast<std::uint32_t>(seed_item.value);
    s.angle = static_cast<int>(angle_item.value);
    s.suppress_iframe = suppress_iframe_item.value;
    s.cache_dir = cache_dir;
    s.width = scene.width;
    s.height = scene.height;
    s.fps_num = scene.rate;
    s.fps_den = scene.scale;
    s.interval = media_object_interval(object);
    const std::int64_t before = media_before_frames();
    s.cut_offset = (before > 0) ? before : -1;
    s.normalize();
    return s;
}

// First finished glitch cache file for these settings, generations
// 0..7. Empty when nothing was generated yet.
std::filesystem::path find_glitch_file(const Settings& settings) {
    Context& ctx = context();
    std::string ffgac_version;
    std::string ffedit_version;
    {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        ffgac_version = ctx.ffgac_version;
        ffedit_version = ctx.ffedit_version;
    }
    const std::string src_hash = compute_src_hash(settings, ffgac_version);
    CacheIndex index(settings.cache_dir);
    for (int gen = 0; gen < kMaxGeneration; ++gen) {
        // The generation is part of the file name; recompute per gen.
        const std::string hash =
            compute_glitch_hash(settings, ffedit_version, gen);
        const std::filesystem::path p =
            index.glitch_path(settings.uid, src_hash, hash, gen);
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    return {};
}

// Fetch one frame from a cache MP4 and set it as the object image.
// Returns false when the frame could not be fetched or converted.
bool apply_cache_frame(FILTER_PROC_VIDEO* video,
                       const std::filesystem::path& file, int local_frame) {
    Context& ctx = context();
    if (ctx.cache == nullptr) {
        log_error(&sdk_logger(), "apply_cache: ctx.cache is null");
        return false;
    }
    CACHE_FILE_IMAGE image = ctx.cache->get_video_file_cache(
        file.wstring().c_str(), 0, local_frame);
    if (!image) {
        log_error(&sdk_logger(), format_string("apply_cache: get_video_file_cache returned null for frame %d of %s",
                                              local_frame, file.filename().string().c_str()));
        return false;
    }
    const int width = image.width;
    const int height = image.height;
    if (width <= 0 || height <= 0 || image.buffer == nullptr) {
        log_error(&sdk_logger(), format_string("apply_cache: invalid image dimensions %dx%d or null buffer", width, height));
        return false;
    }

    std::vector<PIXEL_RGBA> rgba_buffer;
    const PIXEL_RGBA* src_rgba = nullptr;

    if (image.format == INPUT_PIXEL_FORMAT::RGBA) {
        if (image.pitch == width * 4) {
            src_rgba = static_cast<const PIXEL_RGBA*>(image.buffer);
        } else {
            rgba_buffer.resize(static_cast<std::size_t>(width) * height);
            const auto* src = static_cast<const std::uint8_t*>(image.buffer);
            for (int y = 0; y < height; ++y) {
                std::memcpy(rgba_buffer.data() + static_cast<std::size_t>(y) * width,
                            src + static_cast<std::size_t>(y) * image.pitch,
                            static_cast<std::size_t>(width) * 4);
            }
            src_rgba = rgba_buffer.data();
        }
    } else if (image.format == INPUT_PIXEL_FORMAT::BGRA ||
               image.format == INPUT_PIXEL_FORMAT::BGR) {
        rgba_buffer.resize(static_cast<std::size_t>(width) * height);
        const auto* src = static_cast<const std::uint8_t*>(image.buffer);
        for (int y = 0; y < height; ++y) {
            const std::uint8_t* row =
                src + static_cast<std::size_t>(y) * image.pitch;
            for (int x = 0; x < width; ++x) {
                PIXEL_RGBA px{};
                px.b = row[x * 4 + 0];
                px.g = row[x * 4 + 1];
                px.r = row[x * 4 + 2];
                px.a = (image.format == INPUT_PIXEL_FORMAT::BGRA) ? row[x * 4 + 3] : 255;
                rgba_buffer[static_cast<std::size_t>(y) * width + x] = px;
            }
        }
        src_rgba = rgba_buffer.data();
    } else if (image.format == INPUT_PIXEL_FORMAT::YUY2) {
        rgba_buffer.resize(static_cast<std::size_t>(width) * height);
        const auto* src = static_cast<const std::uint8_t*>(image.buffer);
        for (int y = 0; y < height; ++y) {
            const std::uint8_t* row = src + static_cast<std::size_t>(y) * image.pitch;
            for (int x = 0; x < width; x += 2) {
                int y0 = row[x * 2 + 0];
                int u  = row[x * 2 + 1] - 128;
                int y1 = row[x * 2 + 2];
                int v  = row[x * 2 + 3] - 128;
                auto yuv2rgb = [](int Y, int U, int V) -> PIXEL_RGBA {
                    PIXEL_RGBA p{};
                    int r = Y + (91881 * V) / 65536;
                    int g = Y - (22554 * U + 46802 * V) / 65536;
                    int b = Y + (116130 * U) / 65536;
                    p.r = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
                    p.g = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
                    p.b = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
                    p.a = 255;
                    return p;
                };
                rgba_buffer[static_cast<std::size_t>(y) * width + x + 0] = yuv2rgb(y0, u, v);
                if (x + 1 < width) {
                    rgba_buffer[static_cast<std::size_t>(y) * width + x + 1] = yuv2rgb(y1, u, v);
                }
            }
        }
        src_rgba = rgba_buffer.data();
    } else {
        log_error(&sdk_logger(), format_string("apply_cache: unsupported format %d", static_cast<int>(image.format)));
        return false;
    }

    if (src_rgba == nullptr) return false;

    const int target_w = (video->scene != nullptr && video->scene->width > 0)
                             ? video->scene->width
                             : width;
    const int target_h = (video->scene != nullptr && video->scene->height > 0)
                             ? video->scene->height
                             : height;

    if (width == target_w && height == target_h) {
        video->set_image_data(src_rgba, width, height);
    } else {
        std::vector<PIXEL_RGBA> scaled(static_cast<std::size_t>(target_w) * target_h);
        std::span<const std::uint8_t> src_bytes(
            reinterpret_cast<const std::uint8_t*>(src_rgba),
            static_cast<std::size_t>(width) * height * 4);
        std::span<std::uint8_t> dst_bytes(
            reinterpret_cast<std::uint8_t*>(scaled.data()),
            static_cast<std::size_t>(target_w) * target_h * 4);
        fit_frame_bilinear(src_bytes, width, height, dst_bytes, target_w, target_h);
        video->set_image_data(scaled.data(), target_w, target_h);
    }
    return true;
}

} // namespace

bool func_proc_video(FILTER_PROC_VIDEO* video) {
    if (video == nullptr || video->scene == nullptr ||
        video->object == nullptr) {
        return false;
    }
    const SCENE_INFO& scene = *video->scene;
    const OBJECT_INFO& object = *video->object;
    if (scene.width <= 0 || scene.height <= 0) return false;

    // Instance uid: create once, then it persists with the project.
    InstanceData* data = instance_data_item.value;
    std::string uid;
    if (data != nullptr) {
        if (data->uid[0] == '\0') {
            const std::string fresh = make_uid(object.effect_id);
            const std::size_t to_copy =
                std::min(fresh.size(), sizeof(data->uid) - 1);
            std::memcpy(data->uid, fresh.data(), to_copy);
            data->uid[to_copy] = '\0';
        }
        uid = data->uid;
    }
    if (uid.empty()) uid = make_uid(object.effect_id);

    const std::filesystem::path cache_dir =
        resolve_cache_dir(cache_folder_item.value == nullptr
                              ? std::wstring_view{}
                              : std::wstring_view(cache_folder_item.value));

    // Passthrough: show the referenced layer's image unchanged.
    if (passthrough_item.value) {
        const std::wstring layer_resource =
            L"layer:" + std::to_wstring(
                            static_cast<int>(ref_layer_item.value) - 1);
        if (video->copy_image_resource(L"object", layer_resource.c_str())) {
            return true;
        }
        log_warning(&sdk_logger(), "passthrough: layer image not available");
    }

    const Settings settings =
        render_settings(scene, object, uid, cache_dir);
    const std::filesystem::path glitch_file = find_glitch_file(settings);

    if (glitch_file.empty()) {
        static thread_local int miss_count = 0;
        if (++miss_count <= 2) {
            Context& ctx = context();
            std::string ffgac_v, ffedit_v;
            {
                std::lock_guard<std::mutex> lock(ctx.mutex);
                ffgac_v = ctx.ffgac_version;
                ffedit_v = ctx.ffedit_version;
            }
            const std::string src_h = compute_src_hash(settings, ffgac_v);
            const std::string gli_h = compute_glitch_hash(settings, ffedit_v, 0);
            log_warning(&sdk_logger(), format_string("render: glitch file not found for uid=%s, src_h=%s, gli_h=%s, interval=[%lld..%lld], cut_offset=%lld, angle=%d, strength=%d",
                                                     uid.c_str(), src_h.c_str(), gli_h.c_str(),
                                                     static_cast<long long>(settings.interval.start_frame),
                                                     static_cast<long long>(settings.interval.end_frame),
                                                     static_cast<long long>(settings.cut_offset),
                                                     settings.angle, settings.strength));
        }
    } else {
        const int cache_frame = object.frame + static_cast<int>(media_before_frames());
        if (apply_cache_frame(video, glitch_file, cache_frame)) {
            return true;
        }
    }

    if (show_marker_item.value) {
        const int width = object.width > 0 ? object.width : scene.width;
        const int height = object.height > 0 ? object.height : scene.height;
        // The shared_ptr keeps the cached pixels alive while
        // set_image_data uses them, even if another render thread
        // rebuilds the cache for a different size in between.
        const std::shared_ptr<const std::vector<PIXEL_RGBA>> marker =
            marker_cache().get(width, height, make_marker_buffer);
        video->set_image_data(marker->data(), width, height);
    }
    return true;
}

} // namespace hybrid::plugin
