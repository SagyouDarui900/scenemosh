// process.cpp — see process.hpp.

#include "process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hybrid {

namespace {

// Process-wide console flag (Windows only; read by run_windows).
std::atomic<bool> g_show_child_console{false};

} // namespace

void set_show_child_console(bool show) {
    g_show_child_console.store(show);
}

bool show_child_console() {
    return g_show_child_console.load();
}

namespace {

#ifdef _WIN32
// Convert a UTF-8 string to a wide string (CreateProcessW needs one).
std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size);
    return out;
}

// Quote one argument for a Windows command line.
std::wstring quote_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L'"';
    return out;
}

std::wstring build_command_line(const std::filesystem::path& exe,
                                const std::vector<std::string>& args) {
    std::wstring line = quote_arg(exe.wstring());
    for (const std::string& a : args) {
        line += L' ';
        line += quote_arg(utf8_to_wide(a));
    }
    return line;
}

class WinStdinSink final : public StdinSink {
public:
    explicit WinStdinSink(HANDLE h) : handle_(h) {}
    bool write(std::span<const std::uint8_t> bytes) override {
        const std::uint8_t* p = bytes.data();
        std::size_t left = bytes.size();
        while (left > 0) {
            DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(left, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(handle_, p, chunk, &written, nullptr) || written == 0) {
                return false;
            }
            p += written;
            left -= written;
        }
        return true;
    }

private:
    HANDLE handle_;
};

void drain_pipe(HANDLE read_end, std::string& out) {
    std::array<char, 8192> buf{};
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(read_end, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) ||
            n == 0) {
            break;
        }
        out.append(buf.data(), n);
    }
}

ProcessResult run_windows(const std::filesystem::path& executable,
                          const std::vector<std::string>& args,
                          const std::function<bool(StdinSink&)>& writer,
                          int timeout_ms) {
    ProcessResult result;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    HANDLE stderr_read = nullptr, stderr_write = nullptr;
    auto close_pipe = [](HANDLE& h) {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = nullptr;
        }
    };
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0) ||
        !CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        close_pipe(stdin_read);
        close_pipe(stdin_write);
        close_pipe(stdout_read);
        close_pipe(stdout_write);
        close_pipe(stderr_read);
        close_pipe(stderr_write);
        result.error = "failed to create pipes";
        return result;
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    std::wstring command_line = build_command_line(executable, args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    PROCESS_INFORMATION pi{};
    const DWORD creation_flags =
        g_show_child_console.load() ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
    const BOOL ok = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr,
                                   TRUE, creation_flags, nullptr, nullptr, &si, &pi);
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    if (!ok) {
        result.error = "failed to start process";
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        return result;
    }
    result.started = true;

    std::thread out_thread(drain_pipe, stdout_read, std::ref(result.stdout_text));
    std::thread err_thread(drain_pipe, stderr_read, std::ref(result.stderr_text));

    WinStdinSink sink(stdin_write);
    const bool writer_ok = writer ? writer(sink) : true;
    CloseHandle(stdin_write);

    const DWORD wait_ms = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    const DWORD waited = WaitForSingleObject(pi.hProcess, wait_ms);
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        result.timed_out = true;
        result.error = "process timed out";
        WaitForSingleObject(pi.hProcess, INFINITE);
    } else if (!writer_ok) {
        TerminateProcess(pi.hProcess, 1);
        result.error = "stdin writer failed";
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    out_thread.join();
    err_thread.join();
    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    return result;
}

#else // POSIX

class PosixStdinSink final : public StdinSink {
public:
    explicit PosixStdinSink(int fd) : fd_(fd) {}
    bool write(std::span<const std::uint8_t> bytes) override {
        const std::uint8_t* p = bytes.data();
        std::size_t left = bytes.size();
        while (left > 0) {
            const ssize_t n = ::write(fd_, p, left);
            if (n <= 0) return false;
            p += static_cast<std::size_t>(n);
            left -= static_cast<std::size_t>(n);
        }
        return true;
    }

private:
    int fd_;
};

void drain_fd(int fd, std::string& out) {
    std::array<char, 8192> buf{};
    for (;;) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n <= 0) break;
        out.append(buf.data(), static_cast<std::size_t>(n));
    }
}

ProcessResult run_posix(const std::filesystem::path& executable,
                        const std::vector<std::string>& args,
                        const std::function<bool(StdinSink&)>& writer,
                        int timeout_ms) {
    ProcessResult result;
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1}; // reports exec failure (CLOEXEC)
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0 ||
        pipe(err_pipe) != 0) {
        result.error = "failed to create pipes";
        return result;
    }
    fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);

    const pid_t pid = fork();
    if (pid < 0) {
        result.error = "fork failed";
        return result;
    }
    if (pid == 0) {
        // Child: wire pipes to std fds and exec (no shell).
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        close(err_pipe[0]);
        std::vector<const char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(executable.c_str());
        for (const std::string& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        execv(executable.c_str(), const_cast<char* const*>(argv.data()));
        const int saved_errno = errno;
        const ssize_t ignored =
            ::write(err_pipe[1], &saved_errno, sizeof(saved_errno));
        (void)ignored;
        _exit(127);
    }

    close(err_pipe[1]);
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    // exec success closes err_pipe[1] (CLOEXEC) so this read returns 0;
    // exec failure writes errno first.
    int exec_errno = 0;
    {
        std::array<char, sizeof(int)> buf{};
        const ssize_t n = ::read(err_pipe[0], buf.data(), buf.size());
        if (n == static_cast<ssize_t>(sizeof(int))) {
            std::memcpy(&exec_errno, buf.data(), sizeof(int));
        }
    }
    close(err_pipe[0]);
    if (exec_errno != 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        result.error = std::string("failed to start process: ") +
                       std::strerror(exec_errno);
        return result;
    }

    result.started = true;

    std::thread out_thread(drain_fd, stdout_pipe[0], std::ref(result.stdout_text));
    std::thread err_thread(drain_fd, stderr_pipe[0], std::ref(result.stderr_text));

    PosixStdinSink sink(stdin_pipe[1]);
    const bool writer_ok = writer ? writer(sink) : true;
    close(stdin_pipe[1]);

    // Wait with optional timeout.
    int status = 0;
    bool finished = false;
    const auto deadline = timeout_ms < 0
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) { finished = true; break; }
        if (w < 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!finished) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        result.timed_out = true;
        result.error = "process timed out";
    } else if (!writer_ok) {
        result.error = "stdin writer failed";
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    out_thread.join();
    err_thread.join();
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    return result;
}

#endif

} // namespace

ProcessResult run_capture(const std::filesystem::path& executable,
                          const std::vector<std::string>& args,
                          std::optional<std::span<const std::uint8_t>> stdin_data,
                          int timeout_ms) {
    const std::function<bool(StdinSink&)> writer =
        [stdin_data](StdinSink& sink) {
            if (!stdin_data.has_value()) return true;
            return sink.write(*stdin_data);
        };
#ifdef _WIN32
    return run_windows(executable, args, writer, timeout_ms);
#else
    return run_posix(executable, args, writer, timeout_ms);
#endif
}

ProcessResult run_with_stdin(const std::filesystem::path& executable,
                             const std::vector<std::string>& args,
                             const std::function<bool(StdinSink&)>& writer,
                             int timeout_ms) {
#ifdef _WIN32
    return run_windows(executable, args, writer, timeout_ms);
#else
    return run_posix(executable, args, writer, timeout_ms);
#endif
}

} // namespace hybrid
