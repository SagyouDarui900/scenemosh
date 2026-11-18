// ffglitch.cpp — see ffglitch.hpp.

#include "ffglitch.hpp"

#include "process.hpp"

#include <cctype>

namespace hybrid {

std::optional<FFglitchPaths> find_ffglitch(const std::filesystem::path& dir) {
#ifdef _WIN32
    const std::filesystem::path ffgac = dir / "ffgac.exe";
    const std::filesystem::path ffedit = dir / "ffedit.exe";
#else
    const std::filesystem::path ffgac = dir / "ffgac";
    const std::filesystem::path ffedit = dir / "ffedit";
#endif
    std::error_code ec;
    if (!std::filesystem::is_regular_file(ffgac, ec) ||
        !std::filesystem::is_regular_file(ffedit, ec)) {
        return std::nullopt;
    }
    return FFglitchPaths{dir, ffgac, ffedit};
}

std::vector<std::string> build_ffgac_args(int width, int height,
                                          int fps_num, int fps_den,
                                          const std::filesystem::path& output) {
    return {
        "-f", "rawvideo",
        "-pix_fmt", "rgba",
        "-s", std::to_string(width) + "x" + std::to_string(height),
        "-framerate", std::to_string(fps_num) + "/" + std::to_string(fps_den),
        "-i", "-",
        "-an",
        "-vcodec", "mpeg4",
        "-q:v", "2",
        "-g", "max",
        "-sc_threshold", "max",
        "-mpv_flags", "+forcemv+nopimb",
        "-bf", "0",
        "-y", output.string(),
    };
}

std::vector<std::string> build_ffedit_args(const std::string& feature,
                                           const std::filesystem::path& script,
                                           const std::filesystem::path& input,
                                           const std::filesystem::path& output) {
    return {
        "-i", input.string(),
        "-f", feature,
        "-s", script.string(),
        "-o", output.string(),
        "-y",
    };
}

std::optional<std::string> parse_tool_version(const std::string& banner_text) {
    // Look for "ffglitch-X.Y.Z" first, then for "version X.Y.Z".
    const std::string marker = "ffglitch-";
    std::size_t pos = banner_text.find(marker);
    if (pos != std::string::npos) {
        const std::size_t start = pos + marker.size();
        std::size_t end = start;
        while (end < banner_text.size() &&
               (std::isdigit(static_cast<unsigned char>(banner_text[end])) ||
                banner_text[end] == '.')) {
            ++end;
        }
        if (end > start) return banner_text.substr(start, end - start);
    }
    const std::string vmarker = "version ";
    pos = banner_text.find(vmarker);
    if (pos != std::string::npos) {
        const std::size_t start = pos + vmarker.size();
        std::size_t end = start;
        while (end < banner_text.size() &&
               (std::isdigit(static_cast<unsigned char>(banner_text[end])) ||
                banner_text[end] == '.')) {
            ++end;
        }
        if (end > start) return banner_text.substr(start, end - start);
    }
    return std::nullopt;
}

std::optional<std::string> detect_tool_version(const std::filesystem::path& exe) {
    // ffedit/ffgac print their banner to stderr on any invocation.
    // "-version" is not a documented ffglitch flag; invoking with no
    // arguments prints the banner and exits non-zero, which is enough.
    const ProcessResult result = run_capture(exe, {}, std::nullopt, 5000);
    if (!result.started) return std::nullopt;
    if (auto v = parse_tool_version(result.stderr_text)) return v;
    return parse_tool_version(result.stdout_text);
}

} // namespace hybrid
