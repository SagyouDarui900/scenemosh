// timeline_placer.hpp — auto-placement of generated MP4s (M3).
//
// Placement runs inside one edit-section call (call_edit_section), so
// the host groups everything placed in that call into a single Undo
// entry (SDK: "コールバック関数内で編集したオブジェクトは纏めて
// Undoに登録されます"). Whether Undo really covers
// create_object_from_media_file is not documented and must be
// verified on a real machine (README_BUILD.md checklist).
//
// Layer choice (design doc section 7.2): the first free layer above
// the source layer (smaller layer number = higher on the timeline),
// then the user-specified fallback layer, then the first free layer
// below the source layer. A layer is free when no object on it
// overlaps [start_frame, start_frame + frame_count - 1]; the check
// walks the whole layer with find_object so it does not depend on
// what find_object returns for a frame inside an object.
//
// A failed placement never deletes the generated file: the outcome
// carries the file path and the manual steps, and the caller shows
// them in the panel / log.

#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "sdk.hpp"

namespace hybrid::plugin {

struct PlaceRequest {
    std::filesystem::path file; // MP4 to place (must exist)
    int source_layer = 0;       // 0-based layer the segment came from
    int start_frame = 0;        // scene frame where the object starts
    int frame_count = 0;        // object length in frames
    // Object name, UTF-8, e.g. "glitched_amplify_1".
    std::string object_name;
    // 0-based fallback layer, -1 = none (search below the source layer).
    int fallback_layer = -1;
};

struct PlaceOutcome {
    bool ok = false;
    int layer = -1; // layer the object was placed on (0-based)
    // Set when !ok. Includes the file path and the manual steps.
    std::string error;
};

// Place one file. Must be called inside an edit section (the
// callback of call_edit_section_param), never inside a read section.
PlaceOutcome place_media_file(EDIT_SECTION* edit, const PlaceRequest& req);

// Place several files in one edit-section call. Returns one outcome
// per request, in the same order. Returns false only when the edit
// section itself could not be entered (e.g. output in progress);
// in that case every outcome is marked failed.
bool place_media_files(EDIT_HANDLE* edit,
                       const std::vector<PlaceRequest>& requests,
                       std::vector<PlaceOutcome>& outcomes);

} // namespace hybrid::plugin
