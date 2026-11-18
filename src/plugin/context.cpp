// context.cpp — see context.hpp.

#include "context.hpp"

#include <windows.h>

#include <thread>

#include "cache_index.hpp"
#include "downloader.hpp"
#include "ffglitch.hpp"
#include "sdk_logger.hpp"
#include "util.hpp"

namespace hybrid::plugin {

Context& context() {
    static Context instance;
    return instance;
}

void init_context(HOST_APP_TABLE* host) {
    Context& ctx = context();
    ctx.host = host;
    if (host != nullptr) ctx.edit = host->create_edit_handle();
}

void shutdown_context() {
    Context& ctx = context();
    {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        ctx.generator.reset(); // joins the worker thread
        ctx.tools.reset();
    }
    ctx.edit = nullptr;
    ctx.host = nullptr;
    ctx.log_handle = nullptr;
    ctx.config = nullptr;
    ctx.cache = nullptr;
}

void set_log_handle(LOG_HANDLE* handle) {
    context().log_handle = handle;
    sdk_logger().set_handle(handle);
}

void set_config_handle(CONFIG_HANDLE* handle) {
    context().config = handle;
}

void set_cache_handle(CACHE_HANDLE* handle) {
    context().cache = handle;
}

std::filesystem::path app_data_dir() {
    Context& ctx = context();
    if (ctx.config != nullptr && ctx.config->app_data_path != nullptr &&
        ctx.config->app_data_path[0] != L'\0') {
        return std::filesystem::path(ctx.config->app_data_path);
    }
    wchar_t temp[MAX_PATH];
    const DWORD len = ::GetTempPathW(MAX_PATH, temp);
    if (len == 0 || len >= MAX_PATH) return std::filesystem::path(L".");
    std::filesystem::path dir(temp);
    // GetTempPathW ends with a separator; drop it for tidy joins.
    const std::wstring s = dir.wstring();
    if (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) {
        return std::filesystem::path(s.substr(0, s.size() - 1));
    }
    return dir;
}

std::filesystem::path default_cache_dir() {
    return app_data_dir() / L"scenemosh" / L"cache";
}

std::filesystem::path default_tools_dir() {
    return app_data_dir() / L"scenemosh" / L"ffglitch";
}

void set_project_path(std::wstring_view path) {
    std::lock_guard<std::mutex> lock(context().mutex);
    context().project_path = std::wstring(path);
}

std::wstring get_project_path() {
    std::lock_guard<std::mutex> lock(context().mutex);
    return context().project_path;
}

std::filesystem::path resolve_cache_dir(std::wstring_view folder_item) {
    if (!folder_item.empty()) {
        return std::filesystem::path(folder_item);
    }
    const std::wstring proj = get_project_path();
    if (!proj.empty()) {
        std::filesystem::path p(proj);
        if (p.has_parent_path()) {
            return p.parent_path() / L"scenemosh_cache";
        }
    }
    return default_cache_dir();
}

bool confirm_ffglitch_download(HWND parent) {
    const std::filesystem::path tools_dir = default_tools_dir();
    if (find_ffglitch(tools_dir).has_value()) {
        return true;
    }
    const std::wstring msg =
        L"SceneMosh の実行には FFglitch (ffgac.exe / ffedit.exe) が必要です。\n\n"
        L"公式サイト (https://ffglitch.org/) から自動ダウンロード（約40MB）を行いますか？\n\n"
        L"※「いいえ」を選んだ場合、生成は中止されます。\n"
        L"手動で配置したい場合は、以下のフォルダに ffgac.exe と ffedit.exe を配置してください：\n" +
        tools_dir.wstring();
    const int ret = MessageBoxW(
        parent, msg.c_str(), L"SceneMosh - FFglitch ダウンロード確認",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
    return ret == IDYES;
}

void cancel_generator() {
    Context& ctx = context();
    ctx.cancel_pending.store(true);
    std::lock_guard<std::mutex> lock(ctx.mutex);
    if (ctx.generator != nullptr) ctx.generator->cancel();
}

bool submit_to_generator(Job job) {
    Context& ctx = context();
    std::lock_guard<std::mutex> lock(ctx.mutex);
    if (!ctx.tools.has_value()) {
        log_error(&sdk_logger(), "FFglitch tools are not ready yet");
        return false;
    }
    if (ctx.generator == nullptr) {
        ctx.generator =
            std::make_unique<Generator>(*ctx.tools, &sdk_logger());
    }
    ctx.generator->submit(std::move(job));
    return true;
}

void ensure_tools_and_submit(Job job, std::filesystem::path tools_dir) {
    Context& ctx = context();
    // A new submission supersedes an earlier cancel.
    ctx.cancel_pending.store(false);
    {
        std::lock_guard<std::mutex> lock(ctx.mutex);
        if (ctx.tools.has_value()) {
            if (ctx.generator == nullptr) {
                ctx.generator =
                    std::make_unique<Generator>(*ctx.tools, &sdk_logger());
            }
            ctx.generator->submit(std::move(job));
            return;
        }
        if (ctx.ensuring_tools.load()) {
            // A download is already running: queue behind it instead
            // of dropping the job. (The old code logged "job ignored"
            // here, so the job's finished callback never fired and a
            // caller counting on it would wait forever.)
            ctx.queued_jobs.push_back(std::move(job));
            return;
        }
        ctx.ensuring_tools.store(true);
    }

    std::thread([job = std::move(job), tools_dir = std::move(tools_dir)]() mutable {
        Context& ctx = context();
        // Report a result for a job that will not run, so its
        // finished callback still fires exactly once.
        auto drop_job = [](Job& j, bool cancelled, const std::string& error) {
            if (j.finished) {
                JobResult result;
                result.cancelled = cancelled;
                result.error = error;
                j.finished(result);
            }
        };
        auto drop_queued = [&](bool cancelled, const std::string& error) {
            std::vector<Job> queued;
            {
                std::lock_guard<std::mutex> lock(ctx.mutex);
                queued.swap(ctx.queued_jobs);
            }
            for (Job& q : queued) drop_job(q, cancelled, error);
        };
        if (ctx.cancel_pending.load()) {
            drop_job(job, true, "cancelled before start");
            drop_queued(true, "cancelled before start");
            ctx.ensuring_tools.store(false);
            return;
        }
        const bool ok = ensure_ffglitch(tools_dir, &sdk_logger());
        if (!ok) {
            log_error(&sdk_logger(),
                      "FFglitch download failed; see the log above");
            drop_job(job, false, "FFglitch download failed");
            drop_queued(false, "FFglitch download failed");
            ctx.ensuring_tools.store(false);
            return;
        }
        const std::optional<FFglitchPaths> tools = find_ffglitch(tools_dir);
        if (!tools.has_value()) {
            log_error(&sdk_logger(), "FFglitch tools not found after download");
            drop_job(job, false, "FFglitch tools not found after download");
            drop_queued(false, "FFglitch tools not found after download");
            ctx.ensuring_tools.store(false);
            return;
        }
        std::vector<Job> queued;
        {
            std::lock_guard<std::mutex> lock(ctx.mutex);
            ctx.tools = tools;
            ctx.ffgac_version =
                detect_tool_version(tools->ffgac).value_or("unknown");
            ctx.ffedit_version =
                detect_tool_version(tools->ffedit).value_or("unknown");
            if (ctx.generator == nullptr) {
                ctx.generator =
                    std::make_unique<Generator>(*ctx.tools, &sdk_logger());
            }
            queued.swap(ctx.queued_jobs);
            ctx.ensuring_tools.store(false);
        }
        if (ctx.cancel_pending.load()) {
            drop_job(job, true, "cancelled before start");
            for (Job& q : queued) drop_job(q, true, "cancelled before start");
            return;
        }
        ctx.generator->submit(std::move(job));
        for (Job& q : queued) ctx.generator->submit(std::move(q));
    }).detach();
}

ClearCacheResult clear_all_cache() {
    std::vector<std::filesystem::path> dirs = {default_cache_dir()};
    const std::filesystem::path proj_cache = resolve_cache_dir({});
    if (proj_cache != dirs.front()) {
        dirs.push_back(proj_cache);
    }
    ClearCacheResult res;
    std::error_code ec;
    for (const auto& dir : dirs) {
        if (!std::filesystem::exists(dir, ec)) continue;
        for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
            const std::filesystem::path p = item.path();
            if (p.extension() == L".mp4" || p.extension() == L".part" ||
                p.stem().extension() == L".part") {
                if (std::filesystem::remove(p, ec)) {
                    ++res.removed;
                } else if (ec) {
                    ++res.locked;
                }
            }
        }
    }
    if (res.locked > 0) {
        log_warning(&sdk_logger(), format_string("cache cleared: %zu file(s) removed (%zu file(s) in use by AviUtl2)",
                                                 res.removed, res.locked));
    } else {
        log_info(&sdk_logger(), format_string("cache cleared: %zu file(s) removed",
                                              res.removed));
    }
    return res;
}

void remember_rejected_layer(int layer) {
    std::lock_guard<std::mutex> lock(context().mutex);
    context().rejected_layers.insert(layer);
}

bool is_rejected_layer(int layer) {
    std::lock_guard<std::mutex> lock(context().mutex);
    return context().rejected_layers.count(layer) != 0;
}

} // namespace hybrid::plugin
