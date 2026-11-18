// marker_cache.hpp — size-keyed cache for a generated pixel buffer (F2).
//
// func_proc_video used to rebuild the "not generated" marker buffer
// (1920x1080: ~5 ms) on every preview frame, although the picture is
// the same while the size is unchanged. This cache builds it once per
// size and reuses it; a different size rebuilds it.
//
// Thread safety: the host may call func_proc_video from its render
// thread. Callers hold the returned shared_ptr while using the
// buffer, so a rebuild on another thread cannot free the pixels
// under them. The mutex only guards the lookup/rebuild.

#pragma once

#include <memory>
#include <mutex>
#include <vector>

namespace hybrid::plugin {

template <typename Pixel>
class MarkerCache {
public:
    // Returns the cached buffer for (width, height), generating it
    // with `generate` when the size changed (or on first use).
    template <typename Generate>
    std::shared_ptr<const std::vector<Pixel>> get(int width, int height,
                                                  Generate&& generate) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!buffer_ || width_ != width || height_ != height) {
            buffer_ = std::make_shared<const std::vector<Pixel>>(
                generate(width, height));
            width_ = width;
            height_ = height;
        }
        return buffer_;
    }

private:
    std::mutex mutex_;
    int width_ = 0;
    int height_ = 0;
    std::shared_ptr<const std::vector<Pixel>> buffer_;
};

} // namespace hybrid::plugin
