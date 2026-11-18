// settings.hpp — value types for one glitch generation job.
//
// Everything the core needs to reproduce a job is in Settings. The plugin
// copies its UI state into a Settings value and hands it to Generator;
// the core never reads plugin state directly.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hybrid {

// Where the source frames come from.
enum class RefMode {
    Layer,  // frames rendered from a timeline layer
    Screen, // frames captured from the preview framebuffer
};

const char* ref_mode_name(RefMode mode) noexcept;
bool ref_mode_from_name(std::string_view name, RefMode& out) noexcept;

// M1 presets (design doc section 8.1) plus the 12 extra modes from
// section 8.2 (M4). New values are appended after DriftDelta, so the
// existing 0..4 values — which may already be stored in item values —
// do not change.
enum class PresetId {
    Amplify,    // MV増幅
    Fixed,      // MV固定
    Random,     // MVランダム
    Datamosh,   // データモッシュ
    DriftDelta, // MV差分ドリフト
    Zoom,       // ズーム (M4)
    Shear,      // シアー (M4)
    Shift,      // シフト (M4)
    Slice,      // スライス (M4)
    Smear,      // スメア (M4)
    Vibrate,    // バイブレート (M4)
    Mirror,     // ミラー (M4)
    Shuffle,    // シャッフル (M4)
    Buffer,     // バッファ (M4)
    Delay,      // ディレイ (M4)
    Stop,       // ストップ (M4)
    Sink,       // シンク (M4)
};

inline constexpr int kPresetCount = 17;

// Japanese display name, e.g. "MV増幅".
const char* preset_display_name(PresetId preset) noexcept;
// Stable id used in hashes and file names, e.g. "amplify".
const char* preset_id_name(PresetId preset) noexcept;
bool preset_from_id_name(std::string_view name, PresetId& out) noexcept;
// Preset by 0-based index (0..kPresetCount-1), for UI lists.
bool preset_from_index(int index, PresetId& out) noexcept;
// ffedit feature this preset needs: "mv" or "mv_delta".
const char* preset_feature(PresetId preset) noexcept;

// Inclusive frame interval [start_frame, end_frame].
struct Interval {
    std::int64_t start_frame = 0;
    std::int64_t end_frame = -1; // end < start means "empty"

    [[nodiscard]] std::int64_t frame_count() const noexcept {
        return end_frame >= start_frame ? end_frame - start_frame + 1 : 0;
    }
    [[nodiscard]] bool empty() const noexcept { return frame_count() == 0; }
};

struct Settings {
    // Identifies the media object / panel job that owns the cache files.
    // Must be safe to use inside a file name (letters, digits, '-' and '_').
    std::string uid;

    RefMode ref_mode = RefMode::Layer;
    int ref_layer = 0;
    // Frames are read from (interval.start + frame_offset).
    std::int64_t frame_offset = 0;

    PresetId preset = PresetId::Amplify;
    int strength = 50; // 0..100, clamped by normalize()
    std::uint32_t seed = 0;
    // Suppress I-frames in the source encode so every frame is a P-frame.
    bool suppress_iframe = true;

    std::filesystem::path cache_dir;

    int width = 1920;
    int height = 1080;
    int fps_num = 30000;
    int fps_den = 1001;

    Interval interval;

    // Optional libx264 MP4 export (design doc section 10). Off by default.
    bool export_mp4 = false;
    std::string ffmpeg_preset = "fast";
    int crf = 18; // 0..51

    // Clamp every field into its valid range. Returns false only when the
    // uid is empty or contains characters unsafe for file names.
    bool normalize();
    // Human-readable problems; empty means valid. Call normalize() first.
    [[nodiscard]] std::vector<std::string> validate() const;
};

} // namespace hybrid
