// settings.cpp — see settings.hpp.

#include "settings.hpp"

#include <algorithm>

namespace hybrid {

const char* ref_mode_name(RefMode mode) noexcept {
    switch (mode) {
    case RefMode::Layer: return "layer";
    case RefMode::Screen: return "screen";
    }
    return "layer";
}

bool ref_mode_from_name(std::string_view name, RefMode& out) noexcept {
    if (name == "layer") { out = RefMode::Layer; return true; }
    if (name == "screen") { out = RefMode::Screen; return true; }
    return false;
}

const char* preset_display_name(PresetId preset) noexcept {
    switch (preset) {
    case PresetId::Amplify: return "MV増幅";
    case PresetId::Fixed: return "MV固定";
    case PresetId::Random: return "MVランダム";
    case PresetId::Datamosh: return "データモッシュ";
    case PresetId::DriftDelta: return "MV差分ドリフト";
    case PresetId::Zoom: return "ズーム";
    case PresetId::Shear: return "シアー";
    case PresetId::Shift: return "シフト";
    case PresetId::Slice: return "スライス";
    case PresetId::Smear: return "スメア";
    case PresetId::Vibrate: return "バイブレート";
    case PresetId::Mirror: return "ミラー";
    case PresetId::Shuffle: return "シャッフル";
    case PresetId::Buffer: return "バッファ";
    case PresetId::Delay: return "ディレイ";
    case PresetId::Stop: return "ストップ";
    case PresetId::Sink: return "シンク";
    case PresetId::Fluid: return "流体";
    case PresetId::Ripple: return "波紋";
    case PresetId::Gravity: return "重力";
    case PresetId::Transfer: return "モーション転送";
    }
    return "MV増幅";
}

const char* preset_id_name(PresetId preset) noexcept {
    switch (preset) {
    case PresetId::Amplify: return "amplify";
    case PresetId::Fixed: return "fixed";
    case PresetId::Random: return "random";
    case PresetId::Datamosh: return "datamosh";
    case PresetId::DriftDelta: return "drift_delta";
    case PresetId::Zoom: return "zoom";
    case PresetId::Shear: return "shear";
    case PresetId::Shift: return "shift";
    case PresetId::Slice: return "slice";
    case PresetId::Smear: return "smear";
    case PresetId::Vibrate: return "vibrate";
    case PresetId::Mirror: return "mirror";
    case PresetId::Shuffle: return "shuffle";
    case PresetId::Buffer: return "buffer";
    case PresetId::Delay: return "delay";
    case PresetId::Stop: return "stop";
    case PresetId::Sink: return "sink";
    case PresetId::Fluid: return "fluid";
    case PresetId::Ripple: return "ripple";
    case PresetId::Gravity: return "gravity";
    case PresetId::Transfer: return "transfer";
    }
    return "amplify";
}

bool preset_from_id_name(std::string_view name, PresetId& out) noexcept {
    if (name == "amplify") { out = PresetId::Amplify; return true; }
    if (name == "fixed") { out = PresetId::Fixed; return true; }
    if (name == "random") { out = PresetId::Random; return true; }
    if (name == "datamosh") { out = PresetId::Datamosh; return true; }
    if (name == "drift_delta") { out = PresetId::DriftDelta; return true; }
    if (name == "zoom") { out = PresetId::Zoom; return true; }
    if (name == "shear") { out = PresetId::Shear; return true; }
    if (name == "shift") { out = PresetId::Shift; return true; }
    if (name == "slice") { out = PresetId::Slice; return true; }
    if (name == "smear") { out = PresetId::Smear; return true; }
    if (name == "vibrate") { out = PresetId::Vibrate; return true; }
    if (name == "mirror") { out = PresetId::Mirror; return true; }
    if (name == "shuffle") { out = PresetId::Shuffle; return true; }
    if (name == "buffer") { out = PresetId::Buffer; return true; }
    if (name == "delay") { out = PresetId::Delay; return true; }
    if (name == "stop") { out = PresetId::Stop; return true; }
    if (name == "sink") { out = PresetId::Sink; return true; }
    if (name == "fluid") { out = PresetId::Fluid; return true; }
    if (name == "ripple") { out = PresetId::Ripple; return true; }
    if (name == "gravity") { out = PresetId::Gravity; return true; }
    if (name == "transfer") { out = PresetId::Transfer; return true; }
    return false;
}

bool preset_from_index(int index, PresetId& out) noexcept {
    if (index < 0 || index >= kPresetCount) return false;
    out = static_cast<PresetId>(index);
    return true;
}

const char* preset_feature(PresetId preset) noexcept {
    switch (preset) {
    case PresetId::DriftDelta: return "mv_delta";
    default: return "mv";
    }
}

bool preset_supports_angle(PresetId preset) noexcept {
    switch (preset) {
    case PresetId::Fixed:
    case PresetId::DriftDelta:
    case PresetId::Shift:
    case PresetId::Gravity:
    case PresetId::Sink:
    case PresetId::Ripple:
    case PresetId::Transfer:
        return true;
    default:
        return false;
    }
}

namespace {

bool uid_is_safe(const std::string& uid) {
    if (uid.empty()) return false;
    for (char c : uid) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

} // namespace

bool Settings::normalize() {
    strength = std::clamp(strength, 0, 100);
    angle = (angle % 360 + 360) % 360;
    crf = std::clamp(crf, 0, 51);
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (fps_num < 1) fps_num = 1;
    if (fps_den < 1) fps_den = 1;
    if (ffmpeg_preset.empty()) ffmpeg_preset = "fast";
    return uid_is_safe(uid);
}

std::vector<std::string> Settings::validate() const {
    std::vector<std::string> errors;
    if (!uid_is_safe(uid)) errors.emplace_back("uid is empty or unsafe for file names");
    if (cache_dir.empty()) errors.emplace_back("cache_dir is empty");
    if (interval.empty()) errors.emplace_back("interval is empty");
    if (width < 1 || height < 1) errors.emplace_back("width/height must be >= 1");
    if (fps_num < 1 || fps_den < 1) errors.emplace_back("fps numerator/denominator must be >= 1");
    if (strength < 0 || strength > 100) errors.emplace_back("strength must be 0..100");
    if (crf < 0 || crf > 51) errors.emplace_back("crf must be 0..51");
    return errors;
}

} // namespace hybrid
