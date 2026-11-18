// sdk_logger.hpp — hybrid::ILogger implemented on top of LOG_HANDLE.
//
// The SDK limits one log message to 1024 characters; longer messages
// are truncated. Logging from the generator worker thread goes through
// the same handle, guarded by a mutex. When no LOG_HANDLE has been
// supplied (InitializeLogger not called), messages fall back to
// OutputDebugString so they are not silently lost.

#pragma once

#include <windows.h>

#include <mutex>
#include <string_view>

#include "sdk.hpp"
#include "log.hpp"

namespace hybrid::plugin {

class SdkLogger final : public ILogger {
public:
    void set_handle(LOG_HANDLE* handle);

    void log(LogLevel level, std::string_view message) override;

private:
    std::mutex mutex_;
    LOG_HANDLE* handle_ = nullptr; // guarded by mutex_
};

// Process-wide logger instance used by the generator and the plugin.
SdkLogger& sdk_logger();

} // namespace hybrid::plugin
