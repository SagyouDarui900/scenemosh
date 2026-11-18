// util.hpp — small UTF-8 / UTF-16 and formatting helpers for the plugin.
//
// The core works in UTF-8 (std::string). The SDK works in UTF-16
// (LPCWSTR). std::filesystem::path is always built from std::wstring on
// Windows, never from a narrow string, because path(std::string) would
// interpret the bytes in the ANSI code page instead of UTF-8.

#pragma once

#include <string>
#include <string_view>

namespace hybrid::plugin {

// UTF-8 -> UTF-16. Returns an empty string on failure.
std::wstring utf8_to_wide(std::string_view text);
// UTF-16 -> UTF-8. Returns an empty string on failure.
std::string wide_to_utf8(std::wstring_view text);

// printf-style formatting into a std::string.
std::string format_string(const char* format, ...);
// printf-style formatting into a std::wstring (panel list/status text).
std::wstring format_wstring(const wchar_t* format, ...);

} // namespace hybrid::plugin
