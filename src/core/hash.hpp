// hash.hpp — FNV-1a 64-bit hashing for cache file names.
//
// Mixing rules (design doc section 9):
//   - every numeric field is mixed as a fixed 8-byte little-endian value,
//     so "1" and "01" can never collide and field order matters;
//   - every string is mixed length-prefixed (8-byte length, then bytes);
//   - every enum is mixed by its stable name, never by its number, so
//     reordering the enum does not silently change existing cache names.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "settings.hpp"

namespace hybrid {

class Fnv1a64 {
public:
    Fnv1a64() = default;

    void mix_u64(std::uint64_t value) noexcept;
    void mix_u32(std::uint32_t value) noexcept { mix_u64(value); }
    void mix_i64(std::int64_t value) noexcept { mix_u64(static_cast<std::uint64_t>(value)); }
    void mix_bool(bool value) noexcept { mix_u64(value ? 1 : 0); }
    void mix_bytes(std::span<const std::uint8_t> bytes) noexcept;
    void mix_string(std::string_view text) noexcept; // length-prefixed
    void mix_name(std::string_view name) noexcept { mix_string(name); }

    [[nodiscard]] std::uint64_t value() const noexcept { return state_; }
    // 16 lowercase hex digits, zero padded.
    [[nodiscard]] std::string hex() const;

    static constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    static constexpr std::uint64_t kPrime = 1099511628211ULL;

private:
    std::uint64_t state_ = kOffsetBasis;
};

// Source hash: everything that changes the encoded source MP4.
// (design doc section 9: ref mode, ref layer, frame offset, interval,
//  resolution, fps, ffgac version)
std::string compute_src_hash(const Settings& settings, std::string_view ffgac_version);

// Glitch hash: everything that changes the ffedit output, plus the
// generation number used when the output file is locked by playback.
// (design doc section 9)
std::string compute_glitch_hash(const Settings& settings,
                                std::string_view ffedit_version,
                                int generation);

} // namespace hybrid
