// plugin.cpp — the .aux2 entry points.
//
// This DLL is a generic plugin (.aux2). It registers one filter
// plugin, the "SceneMosh" media object (FLAG_VIDEO | FLAG_INPUT), with
// the host. It does not export GetFilterPluginTable, so it is not
// also loaded as a standalone filter plugin (.auf2).
//
// RequiredVersion() is intentionally NOT exported: the host version
// number encoding is not documented (design doc section 14).

#include <windows.h>

#include "sdk.hpp"

#include "context.hpp"
#include "items.hpp"
#include "panel.hpp"
#include "sdk_logger.hpp"

namespace hybrid::plugin {

bool func_proc_video(FILTER_PROC_VIDEO* video);

FILTER_PLUGIN_TABLE filter_plugin_table = {
    FILTER_PLUGIN_TABLE::FLAG_VIDEO | FILTER_PLUGIN_TABLE::FLAG_INPUT,
    L"SceneMosh", // name
    L"SceneMosh", // label
    L"SceneMosh plugin (SceneMosh media object)", // information
    filter_items,
    &func_proc_video,
    nullptr, // func_proc_audio
    nullptr, // func_create (FLAG_USERDATA is not used)
    nullptr, // func_destroy
};

void on_clear_cache(EDIT_SECTION* /*edit*/) {
    clear_all_cache();
}

void on_project_update(PROJECT_FILE* project) {
    if (project != nullptr && project->get_project_file_path != nullptr) {
        LPCWSTR path = project->get_project_file_path();
        set_project_path(path ? path : L"");
    } else {
        set_project_path(L"");
    }
}

} // namespace hybrid::plugin

using namespace hybrid::plugin;

namespace {

// DLL module handle, for the panel window class. Captured in DllMain.
HINSTANCE g_hinstance = nullptr;

} // namespace

// The host loads this DLL with LoadLibrary; DllMain only records the
// module handle and opts out of thread notifications.
BOOL WINAPI DllMain(HINSTANCE hinstance, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinstance = hinstance;
        DisableThreadLibraryCalls(hinstance);
    }
    return TRUE;
}

//---------------------------------------------------------------------
//	Generic plugin registration (required)
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    if (host == nullptr) return;
    host->set_plugin_information(
        L"SceneMosh plugin (SceneMosh media object)");
    init_context(host);
    host->register_filter_plugin(&filter_plugin_table);
    host->register_clear_cache_handler(&on_clear_cache);
    if (host->register_project_load_handler != nullptr) {
        host->register_project_load_handler(&on_project_update);
    }
    if (host->register_project_save_handler != nullptr) {
        host->register_project_save_handler(&on_project_update);
    }
    // The panel is optional: if its window cannot be created, the
    // media object still works.
    create_panel_window(host, g_hinstance);
}

//---------------------------------------------------------------------
//	Plugin DLL initialisation / shutdown (optional)
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD /*version*/) {
    return true;
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    // Destroy the panel first: its window procedure must not run after
    // the context (and its generator worker) is gone.
    destroy_panel_window();
    shutdown_context();
}

//---------------------------------------------------------------------
//	Logger / config / cache handles (optional, called before
//	InitializePlugin when the host supports them)
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* logger) {
    set_log_handle(logger);
}

EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* config) {
    set_config_handle(config);
}

EXTERN_C __declspec(dllexport) void InitializeCache(CACHE_HANDLE* cache) {
    set_cache_handle(cache);
}
