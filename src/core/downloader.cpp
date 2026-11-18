// downloader.cpp — see downloader.hpp.

#include "downloader.hpp"

#include "process.hpp"
#include "sha256.hpp"

#include <array>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#ifdef _MSC_VER
#pragma comment(lib, "wininet.lib")
#endif
#endif

namespace hybrid {

const FFglitchRelease& expected_ffglitch_release() {
    static const FFglitchRelease release;
    return release;
}

bool verify_release_archive(const std::filesystem::path& zip_path,
                            const FFglitchRelease& release) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(zip_path, ec);
    if (ec || size != release.size_bytes) return false;
    return sha256_file_hex(zip_path) == release.sha256;
}

bool download_file(const std::string& url,
                   const std::filesystem::path& dest_path,
                   ILogger* logger) {
#ifdef _WIN32
    HINTERNET session = InternetOpenW(L"scenemosh", INTERNET_OPEN_TYPE_PRECONFIG,
                                      nullptr, nullptr, 0);
    if (session == nullptr) {
        log_error(logger, "download: InternetOpen failed");
        return false;
    }
    std::wstring wide_url;
    {
        const int size = MultiByteToWideChar(CP_UTF8, 0, url.data(),
                                             static_cast<int>(url.size()), nullptr, 0);
        wide_url.assign(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
        if (size > 0) {
            MultiByteToWideChar(CP_UTF8, 0, url.data(), static_cast<int>(url.size()),
                                wide_url.data(), size);
        }
    }
    HINTERNET request = InternetOpenUrlW(session, wide_url.c_str(),
                                         nullptr, 0,
                                         INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (request == nullptr) {
        log_error(logger, "download: InternetOpenUrl failed for " + url);
        InternetCloseHandle(session);
        return false;
    }
    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        log_error(logger, "download: cannot open " + dest_path.string());
        InternetCloseHandle(request);
        InternetCloseHandle(session);
        return false;
    }
    std::array<char, 65536> buf{};
    DWORD read = 0;
    bool ok = true;
    while (InternetReadFile(request, buf.data(), static_cast<DWORD>(buf.size()), &read) &&
           read > 0) {
        out.write(buf.data(), read);
        if (!out) { ok = false; break; }
    }
    InternetCloseHandle(request);
    InternetCloseHandle(session);
    if (!ok) log_error(logger, "download: write failed for " + dest_path.string());
    return ok;
#else
    const ProcessResult result = run_capture(
        "/usr/bin/curl",
        {"-sL", "--fail", "-o", dest_path.string(), url},
        std::nullopt, 600000);
    if (!result.started || result.exit_code != 0) {
        log_error(logger, "download: curl failed for " + url + ": " + result.stderr_text);
        return false;
    }
    return true;
#endif
}

namespace {

bool extract_with_unzip(const std::filesystem::path& zip_path,
                        const std::filesystem::path& dest_dir,
                        ILogger* logger) {
#ifdef _WIN32
    // PowerShell Expand-Archive, run through powershell.exe.
    const std::string script = "Expand-Archive -LiteralPath '" +
        zip_path.string() + "' -DestinationPath '" + dest_dir.string() +
        "' -Force";
    const ProcessResult result = run_capture(
        "powershell.exe",
        {"-NoProfile", "-Command", script}, std::nullopt, 600000);
    if (!result.started || result.exit_code != 0) {
        log_error(logger, "extract: Expand-Archive failed: " + result.stderr_text);
        return false;
    }
    return true;
#else
    const ProcessResult result = run_capture(
        "/usr/bin/unzip",
        {"-o", "-q", zip_path.string(), "-d", dest_dir.string()},
        std::nullopt, 600000);
    if (!result.started || result.exit_code != 0) {
        log_error(logger, "extract: unzip failed: " + result.stderr_text);
        return false;
    }
    return true;
#endif
}

} // namespace

bool extract_ffglitch(const std::filesystem::path& zip_path,
                      const std::filesystem::path& tool_dir,
                      ILogger* logger) {
    std::error_code ec;
    std::filesystem::create_directories(tool_dir, ec);
    const std::filesystem::path staging = tool_dir / "_extract";
    std::filesystem::remove_all(staging, ec);
    if (!extract_with_unzip(zip_path, staging, logger)) {
        std::filesystem::remove_all(staging, ec);
        return false;
    }
#ifdef _WIN32
    const std::string ffgac_name = "ffgac.exe";
    const std::string ffedit_name = "ffedit.exe";
#else
    const std::string ffgac_name = "ffgac";
    const std::string ffedit_name = "ffedit";
#endif
    // Find the tools inside the single top-level folder of the zip.
    std::filesystem::path found_ffgac;
    std::filesystem::path found_ffedit;
    for (const auto& item : std::filesystem::recursive_directory_iterator(staging, ec)) {
        if (ec) break;
        const std::string name = item.path().filename().string();
        if (name == ffgac_name) found_ffgac = item.path();
        if (name == ffedit_name) found_ffedit = item.path();
    }
    bool ok = !found_ffgac.empty() && !found_ffedit.empty();
    if (ok) {
        std::filesystem::rename(found_ffgac, tool_dir / ffgac_name, ec);
        if (ec) ok = false;
        std::filesystem::rename(found_ffedit, tool_dir / ffedit_name, ec);
        if (ec) ok = false;
    }
    if (!ok) log_error(logger, "extract: ffgac/ffedit not found in archive");
    std::filesystem::remove_all(staging, ec);
    return ok;
}

bool ensure_ffglitch(const std::filesystem::path& tool_dir, ILogger* logger) {
    if (find_ffglitch(tool_dir).has_value()) return true;
    const FFglitchRelease& release = expected_ffglitch_release();
    std::error_code ec;
    std::filesystem::create_directories(tool_dir, ec);
    const std::filesystem::path zip_path = tool_dir / "ffglitch.zip";
    log_info(logger, "downloading FFglitch " + release.version + " from " + release.url);
    if (!download_file(release.url, zip_path, logger)) return false;
    if (!verify_release_archive(zip_path, release)) {
        log_error(logger, "downloaded archive failed size/SHA-256 verification");
        std::filesystem::remove(zip_path, ec);
        return false;
    }
    if (!extract_ffglitch(zip_path, tool_dir, logger)) return false;
    std::filesystem::remove(zip_path, ec);
    return find_ffglitch(tool_dir).has_value();
}

} // namespace hybrid
