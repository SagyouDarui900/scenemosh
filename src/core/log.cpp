// log.cpp — see log.hpp.

#include "log.hpp"

namespace hybrid {

const char* log_level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
    }
    return "info";
}

bool VectorLogger::has(LogLevel level, std::string_view substring) const {
    for (const LogEntry& entry : entries_) {
        if (entry.level == level &&
            entry.message.find(substring) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void log_info(ILogger* logger, std::string_view message) {
    if (logger != nullptr) logger->log(LogLevel::Info, message);
}
void log_warning(ILogger* logger, std::string_view message) {
    if (logger != nullptr) logger->log(LogLevel::Warning, message);
}
void log_error(ILogger* logger, std::string_view message) {
    if (logger != nullptr) logger->log(LogLevel::Error, message);
}
void log_debug(ILogger* logger, std::string_view message) {
    if (logger != nullptr) logger->log(LogLevel::Debug, message);
}

} // namespace hybrid
