// sdk_logger.cpp — see sdk_logger.hpp.

#include "sdk_logger.hpp"

#include <windows.h>

#include "util.hpp"

namespace hybrid::plugin {

namespace {

// LOG_HANDLE limits one message to 1024 characters. Keep a margin for
// the terminating null and for any prefix the host adds.
constexpr std::size_t kMaxLogChars = 1000;

} // namespace

void SdkLogger::set_handle(LOG_HANDLE* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    handle_ = handle;
}

void SdkLogger::log(LogLevel level, std::string_view message) {
    std::wstring wide = utf8_to_wide(message);
    if (wide.size() > kMaxLogChars) wide.resize(kMaxLogChars);

    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_ == nullptr) {
        ::OutputDebugStringW(wide.c_str());
        ::OutputDebugStringW(L"\n");
        return;
    }
    switch (level) {
        case LogLevel::Debug: handle_->verbose(handle_, wide.c_str()); break;
        case LogLevel::Info: handle_->info(handle_, wide.c_str()); break;
        case LogLevel::Warning: handle_->warn(handle_, wide.c_str()); break;
        case LogLevel::Error: handle_->error(handle_, wide.c_str()); break;
    }
}

SdkLogger& sdk_logger() {
    static SdkLogger instance;
    return instance;
}

} // namespace hybrid::plugin
