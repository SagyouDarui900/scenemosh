// context.hpp — process-wide plugin state (handles, generator, tools).
//
// Everything the SDK hands us in RegisterPlugin / Initialize* lives
// here, behind one mutex. The generator worker thread, button callbacks
// (UI thread) and func_proc_video (render thread) all read through it.
//
// Lifetime: init_context() runs in RegisterPlugin; shutdown_context()
// runs in UninitializePlugin and joins the generator worker before any
// handle is dropped.

#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "sdk.hpp"

#include "ffglitch.hpp"
#include "generator.hpp"

namespace hybrid::plugin {

struct Context {
    HOST_APP_TABLE* host = nullptr;
    EDIT_HANDLE* edit = nullptr;
    LOG_HANDLE* log_handle = nullptr;
    CONFIG_HANDLE* config = nullptr;
    CACHE_HANDLE* cache = nullptr;

    // Guards generator_, tools_, versions_ and rejected_layers_.
    std::mutex mutex;
    std::unique_ptr<Generator> generator;
    std::optional<FFglitchPaths> tools;
    // Version strings from detect_tool_version(); "unknown" when the
    // tools have not been found yet. func_proc_video uses the same
    // strings to rebuild cache file names, so they must match what
    // Generator detected for the job that wrote the cache.
    std::string ffgac_version = "unknown";
    std::string ffedit_version = "unknown";
    // Layers where a "フレームバッファ" object was found during this
    // session; generation for them is refused without re-scanning.
    std::set<int> rejected_layers;
    // True while a detached thread is downloading / locating FFglitch.
    std::atomic<bool> ensuring_tools{false};
    // Jobs submitted while ensuring_tools is true; the ensure thread
    // submits them once the tools are ready, instead of dropping
    // them (their finished callbacks must still fire exactly once).
    std::vector<Job> queued_jobs;
    // Set by cancel_generator(); a detached ensure thread that finds
    // it set drops its job instead of submitting it. Cleared when a
    // new job is submitted from the UI thread, so a cancel only
    // applies to jobs submitted before it.
    std::atomic<bool> cancel_pending{false};

    // Full path to the loaded .aup2 project file, or empty if unsaved.
    std::wstring project_path;
};

Context& context();

void init_context(HOST_APP_TABLE* host);
void shutdown_context();

void set_project_path(std::wstring_view path);
std::wstring get_project_path();

void set_log_handle(LOG_HANDLE* handle);
void set_config_handle(CONFIG_HANDLE* handle);
void set_cache_handle(CACHE_HANDLE* handle);

// Application data folder from CONFIG_HANDLE, or the temp folder when
// no config handle is available. Always ends without a separator.
std::filesystem::path app_data_dir();
// Default cache folder: <app data>\scenemosh\cache
std::filesystem::path default_cache_dir();
// Default FFglitch tool folder: <app data>\scenemosh\ffglitch
std::filesystem::path default_tools_dir();
// The folder item value (empty = default) resolved to a real folder.
std::filesystem::path resolve_cache_dir(std::wstring_view folder_item);

// Check if FFglitch exists; if not, ask the user with a confirmation dialog.
// Returns true if tools exist or user consents to download, false if declined.
bool confirm_ffglitch_download(HWND parent = nullptr);

// Submit a job to the generator, creating it first when needed.
// tools must already be set; returns false (and logs) otherwise.
bool submit_to_generator(Job job);

// Stop the running job (if any) and drop queued jobs. Also marks
// cancel_pending so a detached FFglitch-ensure thread drops its job
// instead of submitting it after the download.
void cancel_generator();

// Ensure FFglitch tools exist, then submit the job. When the tools are
// already known this submits immediately; otherwise a detached thread
// downloads them (ensure_ffglitch) and submits afterwards, so the UI
// thread never blocks on a ~40MB download.
void ensure_tools_and_submit(Job job, std::filesystem::path tools_dir);

// Delete every file in the default and project cache folders.
// Returns the number of removed files and locked files (in use by AviUtl2).
struct ClearCacheResult {
    std::size_t removed = 0;
    std::size_t locked = 0;
};
ClearCacheResult clear_all_cache();

// Remember that this layer contained a framebuffer object.
void remember_rejected_layer(int layer);
bool is_rejected_layer(int layer);

} // namespace hybrid::plugin
