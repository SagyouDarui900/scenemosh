// frame_fit.hpp — fit one RGBA frame into a different-sized RGBA
// buffer (pure math, no SDK dependency).
//
// The layer renderer returns each object's own image size, so frames
// on either side of a cut can differ in size (design doc section 19).
// Cut-centred segments mix those frames in one MPEG-4 stream, which
// needs one size for the whole job, so every frame is fitted to the
// scene size here instead of failing the encode.

#pragma once

#include <cstdint>
#include <span>

namespace hybrid {

// Copy src (src_width x src_height, RGBA) into dst (dst_width x
// dst_height, RGBA), centred. Areas of dst outside the source are
// black (0, 0, 0, 255). When the source is larger than the
// destination in a dimension, that dimension is centre-cropped. An
// odd size difference puts the extra pixel on the right / bottom:
// the source starts at (dst - src) / 2 in destination coordinates
// (integer division). Both spans must hold width * height * 4 bytes;
// sizes must be positive. Out-of-range input leaves dst unchanged.
void fit_frame_centered(std::span<const std::uint8_t> src, int src_width,
                        int src_height, std::span<std::uint8_t> dst,
                        int dst_width, int dst_height);

// Scale src (src_width x src_height, RGBA) into dst (dst_width x
// dst_height, RGBA) using bilinear interpolation, preserving aspect
// ratio (aspect-fit). Areas of dst outside the scaled image (if
// aspect ratios differ) are filled with opaque black (0, 0, 0, 255).
// When dimensions match, it copies directly. Both spans must hold
// width * height * 4 bytes; sizes must be positive. Out-of-range
// input leaves dst unchanged.
void fit_frame_bilinear(std::span<const std::uint8_t> src, int src_width,
                        int src_height, std::span<std::uint8_t> dst,
                        int dst_width, int dst_height);

} // namespace hybrid
