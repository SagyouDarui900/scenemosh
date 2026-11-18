// encoder.cpp — see encoder.hpp.

#include "encoder.hpp"

#include "cache_index.hpp"
#include "presets.hpp"
#include "process.hpp"

#include <fstream>
#include <vector>

namespace hybrid {

EncodeResult encode_source(IFrameProvider& provider,
                           const Settings& settings,
                           const FFglitchPaths& tools,
                           const std::filesystem::path& output_part,
                           ILogger* logger,
                           const ProgressFn& progress,
                           const std::function<bool()>& should_stop) {
    EncodeResult result;
    result.output = output_part;
    const std::int64_t total = provider.frame_count();
    if (total <= 0) {
        result.error = "no frames to encode";
        return result;
    }
    const std::size_t frame_bytes =
        static_cast<std::size_t>(settings.width) *
        static_cast<std::size_t>(settings.height) * 4;
    std::vector<std::uint8_t> frame(frame_bytes);

    const std::vector<std::string> args = build_ffgac_args(
        settings.width, settings.height, settings.fps_num, settings.fps_den,
        output_part);
    std::int64_t written = 0;
    bool provider_failed = false;
    bool stop_requested = false;
    const ProcessResult proc = run_with_stdin(
        tools.ffgac, args,
        [&](StdinSink& sink) {
            for (std::int64_t i = 0; i < total; ++i) {
                if (should_stop && should_stop()) {
                    stop_requested = true;
                    result.error = "cancelled during encode";
                    return false;
                }
                if (!provider.read_frame(i, frame)) {
                    provider_failed = true;
                    return false;
                }
                if (!sink.write(frame)) return false;
                ++written;
                if (progress) progress(written, total);
            }
            return true;
        },
        -1);
    result.frames_written = written;
    if (stop_requested) {
        // result.error is already "cancelled during encode"; do not
        // let the ffgac exit code below overwrite it, and do not
        // treat the partial file as a success.
        return result;
    }
    if (!proc.started) {
        result.error = "could not start ffgac: " + proc.error;
        return result;
    }
    if (provider_failed) {
        result.error = "frame provider failed";
        return result;
    }
    if (proc.exit_code != 0) {
        result.error = "ffgac exited with code " + std::to_string(proc.exit_code) +
                       ": " + proc.stderr_text;
        return result;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(output_part, ec) ||
        std::filesystem::file_size(output_part, ec) == 0) {
        result.error = "ffgac produced no output";
        return result;
    }
    log_info(logger, "encoded source: " + output_part.string());
    result.ok = true;
    return result;
}

EncodeResult apply_glitch(const std::filesystem::path& src_path,
                          const Settings& settings,
                          const FFglitchPaths& tools,
                          const std::filesystem::path& output_part,
                          ILogger* logger) {
    EncodeResult result;
    result.output = output_part;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(src_path, ec)) {
        result.error = "source file missing: " + src_path.string();
        return result;
    }
    // Write the generated script next to the output, then remove it.
    std::filesystem::path script_path = output_part;
    script_path += ".js";
    {
        std::ofstream script(script_path, std::ios::binary | std::ios::trunc);
        if (!script) {
            result.error = "cannot write script file: " + script_path.string();
            return result;
        }
        script << generate_preset_js(settings);
    }
    const std::vector<std::string> args = build_ffedit_args(
        preset_feature(settings.preset), script_path, src_path, output_part);
    const ProcessResult proc = run_capture(tools.ffedit, args, std::nullopt, -1);
    std::filesystem::remove(script_path, ec);
    if (!proc.started) {
        result.error = "could not start ffedit: " + proc.error;
        return result;
    }
    if (proc.exit_code != 0) {
        result.error = "ffedit exited with code " + std::to_string(proc.exit_code) +
                       ": " + proc.stderr_text;
        return result;
    }
    if (!std::filesystem::is_regular_file(output_part, ec) ||
        std::filesystem::file_size(output_part, ec) == 0) {
        result.error = "ffedit produced no output";
        return result;
    }
    log_info(logger, "applied glitch: " + output_part.string());
    result.ok = true;
    return result;
}

} // namespace hybrid
