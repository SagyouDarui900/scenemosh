// log.hpp — logging interface for scenemosh core.
//
// The core never writes to stdout/stderr or shows UI itself. The caller
// (the .aux2 plugin) supplies an ILogger implementation; tests use
// VectorLogger to inspect what was logged.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hybrid {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
};

const char* log_level_name(LogLevel level) noexcept;

struct LogEntry {
    LogLevel level = LogLevel::Info;
    std::string message;
};

class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void log(LogLevel level, std::string_view message) = 0;
};

// Discards everything. Used when the caller does not care about logs.
class NullLogger final : public ILogger {
public:
    void log(LogLevel, std::string_view) override {}
};

// Keeps every entry in memory. Used by tests.
class VectorLogger final : public ILogger {
public:
    void log(LogLevel level, std::string_view message) override {
        entries_.push_back(LogEntry{level, std::string(message)});
    }

    [[nodiscard]] const std::vector<LogEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] bool has(LogLevel level, std::string_view substring) const;
    void clear() { entries_.clear(); }

private:
    std::vector<LogEntry> entries_;
};

// Convenience helpers that tolerate a null logger pointer.
void log_info(ILogger* logger, std::string_view message);
void log_warning(ILogger* logger, std::string_view message);
void log_error(ILogger* logger, std::string_view message);
void log_debug(ILogger* logger, std::string_view message);

} // namespace hybrid
