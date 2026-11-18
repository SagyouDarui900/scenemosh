// hash.cpp — see hash.hpp.

#include "hash.hpp"

#include <array>

namespace hybrid {

void Fnv1a64::mix_u64(std::uint64_t value) noexcept {
    // Fixed 8-byte little-endian encoding.
    for (int i = 0; i < 8; ++i) {
        state_ ^= static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF);
        state_ *= kPrime;
    }
}

void Fnv1a64::mix_bytes(std::span<const std::uint8_t> bytes) noexcept {
    for (std::uint8_t b : bytes) {
        state_ ^= b;
        state_ *= kPrime;
    }
}

void Fnv1a64::mix_string(std::string_view text) noexcept {
    mix_u64(static_cast<std::uint64_t>(text.size()));
    mix_bytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string Fnv1a64::hex() const {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(16);
    std::uint64_t v = state_;
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[v & 0xF];
        v >>= 4;
    }
    return out;
}

std::string compute_src_hash(const Settings& settings, std::string_view ffgac_version) {
    Fnv1a64 h;
    h.mix_name("src");
    h.mix_name(ref_mode_name(settings.ref_mode));
    h.mix_i64(settings.ref_layer);
    h.mix_i64(settings.frame_offset);
    h.mix_i64(settings.interval.start_frame);
    h.mix_i64(settings.interval.end_frame);
    h.mix_i64(settings.width);
    h.mix_i64(settings.height);
    h.mix_i64(settings.fps_num);
    h.mix_i64(settings.fps_den);
    h.mix_bool(settings.suppress_iframe);
    h.mix_string(ffgac_version);
    return h.hex();
}

std::string compute_glitch_hash(const Settings& settings,
                                std::string_view ffedit_version,
                                int generation) {
    Fnv1a64 h;
    h.mix_name("glitch");
    h.mix_name(preset_id_name(settings.preset));
    h.mix_i64(settings.strength);
    h.mix_u32(settings.seed);
    h.mix_i64(settings.angle);
    h.mix_i64(settings.cut_offset);
    h.mix_bool(settings.suppress_iframe);
    h.mix_string(ffedit_version);
    h.mix_i64(generation);
    return h.hex();
}

} // namespace hybrid
