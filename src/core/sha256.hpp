// sha256.hpp — SHA-256 for verifying downloaded FFglitch archives.
//
// On Windows this uses BCrypt (the platform's own implementation).
// Everywhere else it uses the small self-contained implementation in
// sha256.cpp (standard FIPS 180-4 algorithm, written for this project).

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace hybrid {

// Lowercase hex SHA-256 of a byte buffer.
std::string sha256_hex(std::span<const std::uint8_t> data);
// Lowercase hex SHA-256 of a string's bytes.
std::string sha256_hex(std::string_view text);
// Lowercase hex SHA-256 of a file's contents. Empty string on failure.
std::string sha256_file_hex(const std::filesystem::path& path);

} // namespace hybrid
