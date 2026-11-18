// mp4_exporter.cpp — see mp4_exporter.hpp.

#include "mp4_exporter.hpp"

#include "process.hpp"

namespace hybrid {

std::vector<std::string> build_ffmpeg_args(const std::filesystem::path& input,
                                           const std::filesystem::path& output,
                                           const std::string& ffmpeg_preset,
                                           int crf) {
    return {
        "-y",
        "-i", input.string(),
        "-an",
        "-c:v", "libx264",
        "-preset", ffmpeg_preset,
        "-crf", std::to_string(crf),
        output.string(),
    };
}

bool export_mp4(const std::filesystem::path& input,
                const std::filesystem::path& output_part,
                const Settings& settings,
                const std::filesystem::path& ffmpeg_exe,
                ILogger* logger,
                std::string& error_out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(ffmpeg_exe, ec)) {
        error_out = "ffmpeg not found: " + ffmpeg_exe.string();
        return false;
    }
    const std::vector<std::string> args = build_ffmpeg_args(
        input, output_part, settings.ffmpeg_preset, settings.crf);
    const ProcessResult proc = run_capture(ffmpeg_exe, args, std::nullopt, -1);
    if (!proc.started) {
        error_out = "could not start ffmpeg: " + proc.error;
        return false;
    }
    if (proc.exit_code != 0) {
        error_out = "ffmpeg exited with code " + std::to_string(proc.exit_code) +
                    ": " + proc.stderr_text;
        return false;
    }
    log_info(logger, "exported mp4: " + output_part.string());
    return true;
}

} // namespace hybrid
