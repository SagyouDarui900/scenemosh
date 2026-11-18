// util.cpp — see util.hpp.

#include "util.hpp"

#include <windows.h>

#include <cstdarg>
#include <vector>

namespace hybrid::plugin {

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                          static_cast<int>(text.size()), out.data(), needed);
    return out;
}

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                          static_cast<int>(text.size()), out.data(), needed,
                          nullptr, nullptr);
    return out;
}

std::string format_string(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::va_list args_copy;
    va_copy(args_copy, args);
    const int needed = std::vsnprintf(nullptr, 0, format, args);
    va_end(args);
    if (needed <= 0) {
        va_end(args_copy);
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    std::vsnprintf(out.data(), out.size() + 1, format, args_copy);
    va_end(args_copy);
    return out;
}

std::wstring format_wstring(const wchar_t* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::va_list args_copy;
    va_copy(args_copy, args);
    const int needed = _vscwprintf(format, args);
    va_end(args);
    if (needed <= 0) {
        va_end(args_copy);
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    std::vswprintf(out.data(), out.size() + 1, format, args_copy);
    va_end(args_copy);
    return out;
}

} // namespace hybrid::plugin
