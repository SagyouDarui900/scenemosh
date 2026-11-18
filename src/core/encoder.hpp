// encoder.hpp — ffgac source encode and ffedit glitch application.
//
// The plugin implements IFrameProvider on top of the AviUtl2 SDK
// (M2). The core only knows how to ask for RGBA frames one at a time
// and pipe them into ffgac's stdin — it never reads ahead.
//
// Both functions write to a ".part" path and never rename it; the
// caller (Generator) renames on success.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

#include "ffglitch.hpp"
#include "log.hpp"
#include "settings.hpp"

namespace hybrid {

// Source of RGBA frames for one interval. index is 0-based within the
// interval; rgba must hold width*height*4 bytes.
//
// The frame size is not always the scene size: rendering_object_video
// returns the referenced object's own rendered image, whose size is
// the object's size (e.g. a clip whose resolution differs from the
// scene). A provider that learns the real size from the host may
// report it here; the Generator then uses that size for the whole job
// (hash, ffgac arguments, frame buffer) instead of the scene size.
// The default returns false (size unknown; the caller's size stands).
// read_frame() must serve frames at the probed size once a probe has
// succeeded. Probing renders one frame, so the first frame is
// rendered twice; that cost is one frame render.
class IFrameProvider {
public:
    virtual ~IFrameProvider() = default;
    [[nodiscard]] virtual std::int64_t frame_count() const = 0;
    virtual bool read_frame(std::int64_t index, std::span<std::uint8_t> rgba) = 0;
    virtual bool probe_frame_size(int& width, int& height) {
        (void)width;
        (void)height;
        return false;
    }
};

struct EncodeResult {
    bool ok = false;
    std::string error;
    std::filesystem::path output; // the .part path that was written
    std::int64_t frames_written = 0;
};

// progress(frames_done, frames_total) — called after each frame.
using ProgressFn = std::function<void(std::int64_t, std::int64_t)>;

// Encode the interval into a source MP4 with ffgac.
// output_part is the ".part" destination path.
// should_stop, when set, is checked before every frame; when it
// returns true the encode stops and the result carries the error
// "cancelled during encode" (the caller maps that to a cancelled
// job when its own cancel flag is set).
EncodeResult encode_source(IFrameProvider& provider,
                           const Settings& settings,
                           const FFglitchPaths& tools,
                           const std::filesystem::path& output_part,
                           ILogger* logger,
                           const ProgressFn& progress = nullptr,
                           const std::function<bool()>& should_stop = nullptr);

// Apply the preset to a source MP4 with ffedit.
// output_part is the ".part" destination path. The generated script is
// written next to it as "<stem>.js" and removed afterwards.
EncodeResult apply_glitch(const std::filesystem::path& src_path,
                          const Settings& settings,
                          const FFglitchPaths& tools,
                          const std::filesystem::path& output_part,
                          ILogger* logger);

} // namespace hybrid
