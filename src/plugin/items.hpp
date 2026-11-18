// items.hpp — the SceneMosh media object's settings items.
//
// The FILTER_ITEM_* globals below are the single source of the UI
// state. The host updates each item's .value when it calls
// func_proc_video, and again when a button callback runs (see
// FILTER_ITEM_BUTTON in filter2.h), so button callbacks read the
// current values from these globals.
//
// Per-instance state that must survive a project save/load lives in
// FILTER_ITEM_DATA<InstanceData> (the "データ" item): a stable uid
// used for cache file names. The uid is created in func_proc_video
// the first time the object renders; button callbacks read it back
// with EDIT_SECTION::get_effect_data_value and fall back to a
// session-only uid when the data has not been written yet.
//
// Two-instance caveat (unverified on a real machine): the item globals
// are shared by every SceneMosh object in the project. The host is
// assumed to swap the values in for the object being processed or
// edited, as the SDK sample does with file-scope items, but two
// objects generating at the same time would read each other's values.
// Generation is therefore limited to one job at a time (the core
// Generator already serialises jobs), and each job copies the values
// into a Settings snapshot before submitting.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "sdk.hpp"
#include "settings.hpp"

namespace hybrid::plugin {

// Per-instance data persisted with the project (16KB limit; this is
// far smaller). Written in func_proc_video, read in button callbacks.
struct InstanceData {
    char uid[48] = {}; // e.g. "ffglitch-1a2b3c-0001", empty = not set yet
    int last_generation = 0;
};

extern FILTER_ITEM_SELECT ref_mode_item;       // 0 = layer, 1 = screen
extern FILTER_ITEM_TRACK ref_layer_item;       // 1..100, shown 1-based
extern FILTER_ITEM_TRACK frame_offset_item;    // frames
extern FILTER_ITEM_TRACK before_frames_item;   // lead-in frames, 0..600
extern FILTER_ITEM_TRACK after_frames_item;    // lead-out frames, 0..600
extern FILTER_ITEM_SELECT preset_item;         // 0..16, see settings.hpp
extern FILTER_ITEM_TRACK strength_item;        // 0..100
extern FILTER_ITEM_TRACK seed_item;            // 0..100000
extern FILTER_ITEM_TRACK angle_item;           // 0..360
extern FILTER_ITEM_CHECK suppress_iframe_item;
extern FILTER_ITEM_FOLDER cache_folder_item;   // empty = default folder
extern FILTER_ITEM_CHECK export_mp4_item;      // libx264 MP4 export (off)
extern FILTER_ITEM_FILE ffmpeg_file_item;  // ffmpeg.exe path, empty = search PATH
extern FILTER_ITEM_CHECK passthrough_item;     // show the source instead
extern FILTER_ITEM_CHECK show_marker_item;     // show "not generated" mark
extern FILTER_ITEM_DATA<InstanceData> instance_data_item;
extern FILTER_ITEM_BUTTON generate_button;
extern FILTER_ITEM_BUTTON glitch_only_button;
extern FILTER_ITEM_BUTTON delete_cache_button;
extern FILTER_ITEM_BUTTON delete_unused_button;

// Null-terminated item list for FILTER_PLUGIN_TABLE.
extern void* filter_items[];

// The media object's generation interval: its own span widened by
// the 繋ぎ目前 / 繋ぎ目後 items (design doc section 25), so the
// encode stream includes the frames before the object's start
// (the previous clip) and after its end. The start is clamped to
// frame 0. build_settings() and render_settings() both use this, so
// the generation and the cache lookup hash the same interval.
Interval media_object_interval(const OBJECT_INFO& object);
// Lead-in frames before the object's own start, i.e. the offset
// between a cache frame index and the object's local frame when the
// cache is played back.
std::int64_t media_before_frames();

// Build a uid for an effect instance. Used when the persisted data
// has no uid yet; the session-only fallback is "ffglitch-<effect id>".
std::string make_uid(std::int64_t effect_id);

// Read the persisted uid for an object, or the session-only fallback.
// effect must be the object's SceneMosh effect handle.
std::string read_uid(EDIT_SECTION* edit, OBJECT_HANDLE object,
                     EFFECT_HANDLE effect, std::int64_t effect_id);

// Button callbacks (EDIT_SECTION* is supplied by the host).
void on_generate_button(EDIT_SECTION* edit);
void on_glitch_only_button(EDIT_SECTION* edit);
void on_delete_cache_button(EDIT_SECTION* edit);
void on_delete_unused_button(EDIT_SECTION* edit);

} // namespace hybrid::plugin
