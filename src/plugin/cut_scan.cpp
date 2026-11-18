// cut_scan.cpp — see cut_scan.hpp.

#include "cut_scan.hpp"

#include <windows.h>

#include <algorithm>

#include "sdk_logger.hpp"
#include "util.hpp"

namespace hybrid::plugin {

namespace {

// Hard stop so a corrupt layer (find_object returning the same object
// forever) cannot loop the UI thread forever. A real timeline has far
// fewer objects than this on one layer.
constexpr int kMaxScanObjects = 100000;

struct ScanParam {
    int layer = 0;
    int scan_start = 0;
    int scan_end = -1;
    CutScanResult result;
};

void scan_callback(void* param, EDIT_SECTION* edit) {
    auto* p = static_cast<ScanParam*>(param);
    CutScanResult& out = p->result;
    int frame = p->scan_start;
    int walked = 0;
    while (frame <= p->scan_end && walked < kMaxScanObjects) {
        ++walked;
        OBJECT_HANDLE object = edit->find_object(p->layer, frame);
        if (object == nullptr) break;
        const OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);
        if (lf.start > p->scan_end) break;
        // find_object must move forward; a span ending before the
        // frame we asked for means the host disagrees with us, so
        // stop rather than spin.
        if (lf.end < frame) break;

        bool is_scene = false;
        const int effect_count = edit->get_effect_list(object, nullptr, 0);
        if (effect_count > 0) {
            std::vector<EFFECT_HANDLE> effects(
                static_cast<std::size_t>(effect_count));
            const int got =
                edit->get_effect_list(object, effects.data(), effect_count);
            for (int i = 0; i < got; ++i) {
                const LPCWSTR name = edit->get_effect_name(effects[i]);
                if (name == nullptr) continue;
                if (std::wstring(name) == L"フレームバッファ") {
                    out.found_framebuffer = true;
                    out.completed = false;
                    out.error =
                        "layer contains a framebuffer object; scan stopped";
                    return;
                }
                if (i == 0 && std::wstring(name) == L"シーン") {
                    is_scene = true;
                }
            }
        }

        ScannedSpan span;
        span.span.start_frame =
            std::max<std::int64_t>(lf.start, p->scan_start);
        span.span.end_frame = std::min<std::int64_t>(lf.end, p->scan_end);
        span.is_scene = is_scene;
        out.spans.push_back(span);
        if (is_scene) ++out.scene_object_count;
        frame = lf.end + 1;
    }
    if (walked >= kMaxScanObjects) {
        out.completed = false;
        out.error = "too many objects on the layer; scan stopped";
        return;
    }
    out.completed = true;
}

} // namespace

CutScanResult scan_layer_spans(EDIT_SECTION* edit, int layer,
                               int scan_start, int scan_end) {
    CutScanResult result;
    if (edit == nullptr || scan_end < scan_start) {
        result.error = "invalid scan range";
        return result;
    }
    ScanParam param{layer, scan_start, scan_end, CutScanResult{}};
    scan_callback(&param, edit);
    return param.result;
}

bool scan_cut_segments(EDIT_HANDLE* edit, int layer, int before_frames,
                       int after_frames, int scan_start, int scan_end,
                       std::vector<CutSegment>& out, std::string& error,
                       SegmentMode mode) {
    out.clear();
    error.clear();
    if (edit == nullptr) {
        error = "edit handle is not ready";
        return false;
    }
    if (scan_end < scan_start) {
        error = "invalid scan range";
        return false;
    }
    ScanParam param{layer, static_cast<int>(scan_start),
                    static_cast<int>(scan_end), CutScanResult{}};
    if (!edit->call_read_section_param(&param, &scan_callback)) {
        error = "could not lock the edit section for reading";
        return false;
    }
    const CutScanResult& result = param.result;
    if (result.found_framebuffer) {
        error = result.error;
        log_error(&sdk_logger(), "cut scan: " + error);
        return false;
    }
    if (!result.completed) {
        error = result.error.empty() ? "scan did not complete"
                                     : result.error;
        log_error(&sdk_logger(), "cut scan: " + error);
        return false;
    }
    if (result.scene_object_count > 0) {
        log_info(&sdk_logger(),
                 format_string("cut scan: %d scene object(s) treated as "
                               "single spans (inner sections are not cuts)",
                               result.scene_object_count));
    }
    std::vector<ObjectSpan> spans;
    spans.reserve(result.spans.size());
    for (const ScannedSpan& s : result.spans) spans.push_back(s.span);
    out = detect_segments_in_mode(spans, mode, before_frames, after_frames,
                                  scan_start, scan_end);
    log_info(&sdk_logger(),
             format_string("cut scan: %zu object(s) on layer %d -> %zu "
                           "segment(s) (%s)",
                           result.spans.size(), layer + 1, out.size(),
                           mode == SegmentMode::ObjectBased ? "object-based"
                                                            : "cut-centred"));
    return true;
}

} // namespace hybrid::plugin
