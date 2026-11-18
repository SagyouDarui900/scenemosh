// sdk_source_reader.hpp — IFrameProvider implementations on the SDK.
//
// Layer mode renders one object with EDIT_HANDLE::rendering_object_video;
// screen mode renders the whole scene with
// EDIT_HANDLE::rendering_scene_video. Both calls are asynchronous: the
// host queues the task and calls us back on its event thread, so each
// read_frame() waits on a condition variable with a timeout.
//
// The request state is held in a shared_ptr that is also owned by the
// callback's heap-allocated holder, so a late callback can never touch
// freed memory after read_frame() timed out and returned.
//
// Threading note (unverified on a real machine): the generator worker
// thread calls these functions. Whether rendering_* may be called from
// a non-UI thread is not documented in the SDK headers; M2 must be
// verified on Windows (see README_BUILD.md).

#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "sdk.hpp"

#include "encoder.hpp"
#include "log.hpp"

namespace hybrid::plugin {

// Shared state for one asynchronous render request.
struct RenderRequest {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> callback_fired{false};
    bool done = false;
    bool ok = false;
    int width = 0;
    int height = 0;
    // PIXEL_RGBA bytes, width*height*4.
    std::vector<std::uint8_t> rgba;
};

// Callback for EDIT_HANDLE::rendering_object_video /
// rendering_scene_video. param is a heap-allocated
// std::shared_ptr<RenderRequest>; the callback deletes the holder.
void on_rendering_done(void* param, int frame, const void* buffer,
                       int width, int height, int pitch);

// Render one layer object at one scene frame.
//
// Frames are fitted to the scene size given in the constructor:
// rendering_object_video returns each object's own image size, so
// the frames on either side of a cut can differ, and a cut-centred
// segment mixes them in one MPEG-4 stream (design doc section 25).
// A frame smaller than the scene size is centred with black padding;
// a larger one is centre-cropped (see frame_fit.hpp). The fit and
// the resulting size are logged once per distinct rendered size.
// A scene frame with no object on the layer (a gap, e.g. when the
// object before a cut is shorter than the lead-in) yields a black
// frame instead of failing the whole job; that is logged once.
// No size probe is done: the scene size is the job size, so the
// first frame is not rendered twice (the section 19 probe is
// retired for layer mode; Generator still probes providers that
// report a size).
class LayerSourceReader final : public IFrameProvider {
public:
    LayerSourceReader(EDIT_HANDLE* edit, int layer, std::int64_t base_frame,
                      std::int64_t frame_count, int width, int height,
                      ILogger* logger = nullptr);

    [[nodiscard]] std::int64_t frame_count() const override {
        return frame_count_;
    }
    bool read_frame(std::int64_t index,
                    std::span<std::uint8_t> rgba) override;
    // Always false: the scene size is the job size and every frame
    // is fitted to it in read_frame, so there is no size to report.
    // (Generator keeps the caller's size when this returns false.)
    bool probe_frame_size(int& width, int& height) override;

private:
    EDIT_HANDLE* edit_;
    int layer_;
    std::int64_t base_frame_;
    std::int64_t frame_count_;
    int width_;
    int height_;
    ILogger* logger_;
    // Rendered sizes already logged (one log line per size).
    std::set<std::pair<int, int>> logged_sizes_;
    bool gap_logged_ = false;
};

// Render the whole scene (framebuffer) at one scene frame.
class ScreenSourceReader final : public IFrameProvider {
public:
    ScreenSourceReader(EDIT_HANDLE* edit, std::int64_t base_frame,
                       std::int64_t frame_count, int width, int height,
                       ILogger* logger = nullptr);

    [[nodiscard]] std::int64_t frame_count() const override {
        return frame_count_;
    }
    bool read_frame(std::int64_t index,
                    std::span<std::uint8_t> rgba) override;

private:
    EDIT_HANDLE* edit_;
    std::int64_t base_frame_;
    std::int64_t frame_count_;
    int width_;
    int height_;
    ILogger* logger_;
};

// Check, inside a read section, whether any object on the given layer
// inside [start_frame, end_frame] carries an effect named
// "フレームバッファ". Such a layer must not be rendered (design doc
// section 14: the host can crash). Returns true when one was found.
bool layer_has_framebuffer(EDIT_SECTION* edit, int layer, int start_frame,
                           int end_frame);

} // namespace hybrid::plugin
