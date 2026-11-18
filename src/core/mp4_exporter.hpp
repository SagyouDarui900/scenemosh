// mp4_exporter.hpp — optional libx264 MP4 export (design doc section 10).
//
// Off by default. ffmpeg.exe is never bundled; the caller supplies the
// path to a user-provided ffmpeg. When ffmpeg is missing, export fails
// with an error and the cache MP4 is left untouched.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "log.hpp"
#include "settings.hpp"

namespace hybrid {

// ffmpeg command line (without the executable itself):
//   -y -i <input> -an -c:v libx264 -preset <preset> -crf <crf> <output>
std::vector<std::string> build_ffmpeg_args(const std::filesystem::path& input,
                                           const std::filesystem::path& output,
                                           const std::string& ffmpeg_preset,
                                           int crf);

// Run ffmpeg. output_part is the ".part" destination; the caller renames
// on success. Returns false and fills error on failure.
bool export_mp4(const std::filesystem::path& input,
                const std::filesystem::path& output_part,
                const Settings& settings,
                const std::filesystem::path& ffmpeg_exe,
                ILogger* logger,
                std::string& error_out);

} // namespace hybrid
