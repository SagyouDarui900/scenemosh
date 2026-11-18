// downloader.hpp — fetch and verify the official FFglitch release.
//
// The plugin never bundles FFglitch (GPL). On first use it downloads
// the official Windows zip from ffglitch.org, checks its size and
// SHA-256 against the values measured from the official site, and
// extracts ffgac.exe / ffedit.exe into the tool directory.
//
// Downloading is done with WinINet on Windows and with the `curl`
// command-line tool elsewhere (curl is only a download helper here;
// the plugin itself does not link libcurl).

#pragma once

#include <filesystem>
#include <string>

#include "ffglitch.hpp"
#include "log.hpp"

namespace hybrid {

// The release this project targets (values measured 2026-11-18).
const FFglitchRelease& expected_ffglitch_release();

// True when the file exists, has the expected size, and its SHA-256
// matches the expected value.
bool verify_release_archive(const std::filesystem::path& zip_path,
                            const FFglitchRelease& release);

// Download url to dest_path (overwrites). Returns false on failure and
// logs the reason.
bool download_file(const std::string& url,
                   const std::filesystem::path& dest_path,
                   ILogger* logger);

// Extract ffgac/ffedit from a verified zip into tool_dir.
// The zip contains one top-level folder; the tools are moved out of it.
// Returns false on failure and logs the reason.
bool extract_ffglitch(const std::filesystem::path& zip_path,
                      const std::filesystem::path& tool_dir,
                      ILogger* logger);

// Full flow: if the tools are already present, do nothing; otherwise
// download the archive to a temp file, verify it, extract it, and
// delete the archive. Returns the tool paths on success.
bool ensure_ffglitch(const std::filesystem::path& tool_dir,
                     ILogger* logger);

} // namespace hybrid
