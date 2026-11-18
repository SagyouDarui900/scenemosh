// frame_fit.cpp — see frame_fit.hpp.

#include "frame_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace hybrid {

void fit_frame_centered(std::span<const std::uint8_t> src, int src_width,
                        int src_height, std::span<std::uint8_t> dst,
                        int dst_width, int dst_height) {
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0 ||
        dst_height <= 0) {
        return;
    }
    const std::size_t src_bytes =
        static_cast<std::size_t>(src_width) * src_height * 4;
    const std::size_t dst_bytes =
        static_cast<std::size_t>(dst_width) * dst_height * 4;
    if (src.size() < src_bytes || dst.size() < dst_bytes) return;

    // Black background (opaque).
    for (std::size_t i = 0; i + 4 <= dst.size(); i += 4) {
        dst[i + 0] = 0;
        dst[i + 1] = 0;
        dst[i + 2] = 0;
        dst[i + 3] = 255;
    }

    // Copy rectangle: centre the source in the destination, cropping
    // the source when it is larger in a dimension.
    const int src_x0 = std::max(0, (src_width - dst_width) / 2);
    const int src_y0 = std::max(0, (src_height - dst_height) / 2);
    const int dst_x0 = std::max(0, (dst_width - src_width) / 2);
    const int dst_y0 = std::max(0, (dst_height - src_height) / 2);
    const int copy_w = std::min(src_width - src_x0, dst_width - dst_x0);
    const int copy_h = std::min(src_height - src_y0, dst_height - dst_y0);
    if (copy_w <= 0 || copy_h <= 0) return;
    const std::size_t row_bytes = static_cast<std::size_t>(copy_w) * 4;
    for (int y = 0; y < copy_h; ++y) {
        const std::uint8_t* row_src =
            src.data() + (static_cast<std::size_t>(src_y0 + y) * src_width +
                          src_x0) *
                             4;
        std::uint8_t* row_dst =
            dst.data() + (static_cast<std::size_t>(dst_y0 + y) * dst_width +
                          dst_x0) *
                             4;
        std::memcpy(row_dst, row_src, row_bytes);
    }
}

void fit_frame_bilinear(std::span<const std::uint8_t> src, int src_width,
                        int src_height, std::span<std::uint8_t> dst,
                        int dst_width, int dst_height) {
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0 ||
        dst_height <= 0) {
        return;
    }
    const std::size_t src_bytes =
        static_cast<std::size_t>(src_width) * src_height * 4;
    const std::size_t dst_bytes =
        static_cast<std::size_t>(dst_width) * dst_height * 4;
    if (src.size() < src_bytes || dst.size() < dst_bytes) return;

    if (src_width == dst_width && src_height == dst_height) {
        std::memcpy(dst.data(), src.data(), dst_bytes);
        return;
    }

    const double scale =
        std::min(static_cast<double>(dst_width) / src_width,
                 static_cast<double>(dst_height) / src_height);
    const int scaled_w = std::clamp(
        static_cast<int>(std::lround(src_width * scale)), 1, dst_width);
    const int scaled_h = std::clamp(
        static_cast<int>(std::lround(src_height * scale)), 1, dst_height);
    const int off_x = (dst_width - scaled_w) / 2;
    const int off_y = (dst_height - scaled_h) / 2;

    if (off_x > 0 || off_y > 0 || scaled_w < dst_width ||
        scaled_h < dst_height) {
        for (std::size_t i = 0; i + 4 <= dst.size(); i += 4) {
            dst[i + 0] = 0;
            dst[i + 1] = 0;
            dst[i + 2] = 0;
            dst[i + 3] = 255;
        }
    }

    struct XWeight {
        int x0;
        int x1;
        std::uint32_t wx1;
        std::uint32_t wx0;
    };
    std::vector<XWeight> x_weights(static_cast<std::size_t>(scaled_w));
    for (int x = 0; x < scaled_w; ++x) {
        const double sx =
            (static_cast<double>(x) + 0.5) * src_width / scaled_w - 0.5;
        const double clamped_sx =
            std::clamp(sx, 0.0, static_cast<double>(src_width - 1));
        const int x0 = static_cast<int>(std::floor(clamped_sx));
        const int x1 = std::min(x0 + 1, src_width - 1);
        const auto wx1 = static_cast<std::uint32_t>(
            std::lround((clamped_sx - x0) * 256.0));
        x_weights[static_cast<std::size_t>(x)] = {x0, x1, wx1, 256 - wx1};
    }

    for (int y = 0; y < scaled_h; ++y) {
        const double sy =
            (static_cast<double>(y) + 0.5) * src_height / scaled_h - 0.5;
        const double clamped_sy =
            std::clamp(sy, 0.0, static_cast<double>(src_height - 1));
        const int y0 = static_cast<int>(std::floor(clamped_sy));
        const int y1 = std::min(y0 + 1, src_height - 1);
        const auto wy1 = static_cast<std::uint32_t>(
            std::lround((clamped_sy - y0) * 256.0));
        const std::uint32_t wy0 = 256 - wy1;

        const std::uint8_t* row0 =
            src.data() + static_cast<std::size_t>(y0) * src_width * 4;
        const std::uint8_t* row1 =
            src.data() + static_cast<std::size_t>(y1) * src_width * 4;
        std::uint8_t* dst_row =
            dst.data() +
            static_cast<std::size_t>(off_y + y) * dst_width * 4 +
            static_cast<std::size_t>(off_x) * 4;

        for (int x = 0; x < scaled_w; ++x) {
            const XWeight& xw = x_weights[static_cast<std::size_t>(x)];
            const std::uint8_t* p00 = row0 + static_cast<std::size_t>(xw.x0) * 4;
            const std::uint8_t* p10 = row0 + static_cast<std::size_t>(xw.x1) * 4;
            const std::uint8_t* p01 = row1 + static_cast<std::size_t>(xw.x0) * 4;
            const std::uint8_t* p11 = row1 + static_cast<std::size_t>(xw.x1) * 4;
            std::uint8_t* out = dst_row + static_cast<std::size_t>(x) * 4;

            for (int c = 0; c < 4; ++c) {
                const std::uint32_t top =
                    p00[c] * xw.wx0 + p10[c] * xw.wx1;
                const std::uint32_t bot =
                    p01[c] * xw.wx0 + p11[c] * xw.wx1;
                const std::uint32_t val =
                    (top * wy0 + bot * wy1 + 32768) >> 16;
                out[c] = static_cast<std::uint8_t>(
                    std::min<std::uint32_t>(val, 255));
            }
        }
    }
}

} // namespace hybrid
