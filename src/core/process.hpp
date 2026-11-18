// process.hpp — run an external process with piped stdin/stdout/stderr.
//
// Used to run ffgac, ffedit and ffmpeg as child processes. The core
// never uses a shell: on POSIX the argument vector is passed to exec
// directly, on Windows it is quoted into one command line.
//
// Two entry points:
//   - run_capture(): feed a fixed stdin buffer, capture stdout/stderr.
//   - run_with_stdin(): the caller streams bytes (video frames) through
//     StdinSink while the child runs; returning false from the writer
//     aborts the child.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hybrid {

struct ProcessResult {
    bool started = false; // false when the executable could not be launched
    int exit_code = -1;   // child exit code when started and finished
    bool timed_out = false;
    std::string stdout_text;
    std::string stderr_text;
    std::string error; // human-readable reason when !started or timed_out
};

// Write end of the child's stdin. Returns false on failure.
class StdinSink {
public:
    virtual ~StdinSink() = default;
    virtual bool write(std::span<const std::uint8_t> bytes) = 0;
    virtual bool write(std::string_view text) {
        return write(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }
};

// Show a console window for child processes (ffgac / ffedit / ffmpeg)
// on Windows. Off by default (CREATE_NO_WINDOW). The panel's
// "console" checkbox toggles this so the user can watch the tools run.
// No effect on POSIX, where children never get a console window.
void set_show_child_console(bool show);
bool show_child_console();

// Run a process, feed it stdin_data (if any), wait for it, capture output.
// timeout_ms < 0 means no timeout.
ProcessResult run_capture(const std::filesystem::path& executable,
                          const std::vector<std::string>& args,
                          std::optional<std::span<const std::uint8_t>> stdin_data,
                          int timeout_ms = -1);

// Run a process and let writer stream stdin. writer receives the sink and
// returns true on success; false aborts (the child is killed).
// timeout_ms < 0 means no timeout.
ProcessResult run_with_stdin(const std::filesystem::path& executable,
                             const std::vector<std::string>& args,
                             const std::function<bool(StdinSink&)>& writer,
                             int timeout_ms = -1);

} // namespace hybrid
