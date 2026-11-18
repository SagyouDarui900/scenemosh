// ffglitch.hpp — locating FFglitch and building its command lines.
//
// FFglitch itself is GPL software; this project does not bundle it.
// The plugin downloads the official release (see downloader.hpp) and
// calls ffgac / ffedit as external processes.
//
// ffgac arguments follow design doc section 4: raw RGBA on stdin,
// MPEG-4 part 2, quality 2, no scene-cut keyframes, forced motion
// vectors, no B-frames. rgba is used because AviUtl2's PIXEL_RGBA is
// r,g,b,a in memory; bgra would swap the colour channels.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hybrid {

// The FFglitch release this project targets. Values were measured from
// the official download on 2026-11-18 (see downloader.cpp):
//   size 40311219 bytes,
//   sha256 e5190462eafe9c28428d39f8392a6be93dfd6ac2d59ce08848edec79acf965ca
struct FFglitchRelease {
    std::string version = "0.10.2";
    std::string url =
        "https://ffglitch.org/pub/bin/win64/ffglitch-0.10.2-windows-x86_64.zip";
    std::string sha256 =
        "e5190462eafe9c28428d39f8392a6be93dfd6ac2d59ce08848edec79acf965ca";
    std::uint64_t size_bytes = 40311219;
};

struct FFglitchPaths {
    std::filesystem::path dir;
    std::filesystem::path ffgac;
    std::filesystem::path ffedit;
};

// Look for ffgac/ffedit inside dir. Returns nullopt when either is missing.
std::optional<FFglitchPaths> find_ffglitch(const std::filesystem::path& dir);

// ffgac command line (without the executable itself).
// output is the .src.mp4 path (a .part path while writing).
std::vector<std::string> build_ffgac_args(int width, int height,
                                          int fps_num, int fps_den,
                                          const std::filesystem::path& output);

// ffedit command line (without the executable itself).
std::vector<std::string> build_ffedit_args(const std::string& feature,
                                           const std::filesystem::path& script,
                                           const std::filesystem::path& input,
                                           const std::filesystem::path& output);

// Parse a version number out of ffedit/ffgac banner text, e.g.
//   "ffedit version ffglitch-0.10.2 Copyright ..." -> "0.10.2"
//   "ffedit version 0.10.2 Copyright ..."        -> "0.10.2"
// Returns nullopt when no version can be found.
std::optional<std::string> parse_tool_version(const std::string& banner_text);

// Run "<exe> -version"-style detection is the caller's job; this helper
// only parses text. See encoder.cpp for how versions are obtained.
std::optional<std::string> detect_tool_version(const std::filesystem::path& exe);

} // namespace hybrid
