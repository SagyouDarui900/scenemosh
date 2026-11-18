// panel.hpp — the SceneMosh panel window (M3).
//
// The SDK has no panel registration API. The panel is a plain Win32
// child window created by this plugin and handed to the host with
// HOST_APP_TABLE::register_window_client (design doc section 11.2:
// the host adds WS_CHILD and sets the parent). All controls are
// standard Win32 controls; no common-controls library is needed.
//
// The panel is optional: if its window cannot be created, the plugin
// still works through the SceneMosh media object (M2). Detection and
// generation from the panel run on the UI thread for SDK calls and on
// the core Generator worker for rendering, exactly like the media
// object buttons. Job completion is reported back to the window with
// a posted message, never by touching the HWND from the worker thread.

#pragma once

#include <windows.h>

#include "sdk.hpp"

namespace hybrid::plugin {

// Create the panel window (parented to the host app window) and
// register it as a window client. Returns nullptr on failure.
HWND create_panel_window(HOST_APP_TABLE* host, HINSTANCE instance);
void destroy_panel_window();
HWND panel_window();

} // namespace hybrid::plugin
