// cut_scan.hpp — cut detection on the SDK side (M3).
//
// The core cut_detector (src/core) does the pure frame math; this
// module walks the timeline with EDIT_SECTION::find_object and hands
// it the object spans. Two rules from the design doc (section 6):
//   - a scene object is one span: the sections inside it
//     (get_object_section_frame) are midpoints, not cuts, so they are
//     never split out;
//   - a "フレームバッファ" object on the target layer stops the scan
//     (the same guard the generator uses, design doc section 14).
//
// The walk runs inside one read section (call_read_section_param), so
// it only reads: find_object / get_object_layer_frame / get_effect_list
// / get_effect_name. It never holds the read lock long: one linear
// pass over the layer, no rendering, no per-frame work.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "sdk.hpp"

#include "cut_detector.hpp"

namespace hybrid::plugin {

// One object found on the target layer, plus what the scan learned
// about it. is_scene is true when the object's first effect is named
// "シーン" (best-effort detection; the name is not documented in the
// SDK headers and must be verified on a real machine).
struct ScannedSpan {
    ObjectSpan span;
    bool is_scene = false;
};

struct CutScanResult {
    std::vector<ScannedSpan> spans;
    bool found_framebuffer = false;
    int scene_object_count = 0;
    // False when the scan stopped early (framebuffer found).
    bool completed = false;
    std::string error;
};

// Walk the target layer inside one read section. Collects the spans
// in [scan_start, scan_end], clamped to that range. Returns the spans
// found so far even when the scan stopped early.
CutScanResult scan_layer_spans(EDIT_SECTION* edit, int layer,
                               int scan_start, int scan_end);

// Run the whole scan (read lock + core segment detection) and return
// the glitch segments in the requested mode (design doc section 26).
// Returns false when nothing could be scanned (edit handle missing,
// read lock refused, framebuffer found); the reason is written to
// error.
bool scan_cut_segments(EDIT_HANDLE* edit, int layer, int before_frames,
                       int after_frames, int scan_start, int scan_end,
                       std::vector<CutSegment>& out, std::string& error,
                       SegmentMode mode = SegmentMode::CutCentred);

} // namespace hybrid::plugin
