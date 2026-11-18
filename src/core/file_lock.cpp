// file_lock.cpp — see file_lock.hpp.

#include "file_lock.hpp"

#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace hybrid {

bool can_replace(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
#else
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    ::close(fd);
    return true;
#endif
}

} // namespace hybrid
