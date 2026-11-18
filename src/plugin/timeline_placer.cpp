#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "timeline_placer.hpp"

#include <windows.h>

#include <algorithm>

#include "sdk_logger.hpp"
#include "util.hpp"

namespace hybrid::plugin {

namespace {

// How far above/below the source layer the search goes when no fallback
// layer was given. Layers are cheap to scan (one find_object walk each),
// but an unbounded search could walk a huge empty timeline one layer at a time.
constexpr int kMaxLayersBelow = 64;
constexpr int kMaxLayersAbove = 64;

bool layer_is_free(EDIT_SECTION* edit, int layer, int start_frame,
                   int end_frame) {
    int frame = 0;
    int walked = 0;
    while (walked < 100000) {
        ++walked;
        OBJECT_HANDLE object = edit->find_object(layer, frame);
        if (object == nullptr) return true;
        const OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);
        if (lf.start > end_frame) return true;
        if (lf.end >= start_frame) return false; // overlaps our range
        if (lf.end < frame) return true;          // host disagrees; give up
        frame = lf.end + 1;
    }
    return false;
}

std::string manual_steps(const std::filesystem::path& file, int start_frame,
                         int frame_count) {
    return format_string(
        "placement failed for %s (start frame %d, %d frame(s)). "
        "The file was kept. Place it by hand: add a media object "
        "from the file at frame %d with length %d.",
        file.string().c_str(), start_frame, frame_count, start_frame,
        frame_count);
}

struct PlaceParam {
    const std::vector<PlaceRequest>* requests = nullptr;
    std::vector<PlaceOutcome>* outcomes = nullptr;
};

void place_callback(void* param, EDIT_SECTION* edit) {
    auto* p = static_cast<PlaceParam*>(param);
    for (std::size_t i = 0; i < p->requests->size(); ++i) {
        (*p->outcomes)[i] = place_media_file(edit, (*p->requests)[i]);
    }
}

} // namespace

PlaceOutcome place_media_file(EDIT_SECTION* edit, const PlaceRequest& req) {
    PlaceOutcome outcome;
    if (edit == nullptr) {
        outcome.error = "edit section is not available";
        return outcome;
    }
    if (req.file.empty() || req.frame_count <= 0) {
        outcome.error = "invalid placement request";
        return outcome;
    }
    std::error_code ec;
    if (!std::filesystem::exists(req.file, ec)) {
        outcome.error = format_string("file to place does not exist: %s",
                                      req.file.string().c_str());
        return outcome;
    }
    const int start = req.start_frame;
    const int end = req.start_frame + req.frame_count - 1;

    // 1. First free layer above the source layer (smaller number).
    int layer = -1;
    const int min_above = (std::max)(0, req.source_layer - kMaxLayersAbove);
    for (int l = req.source_layer - 1; l >= min_above; --l) {
        if (layer_is_free(edit, l, start, end)) {
            layer = l;
            break;
        }
    }
    // 2. The user-specified fallback layer.
    if (layer < 0 && req.fallback_layer >= 0 &&
        layer_is_free(edit, req.fallback_layer, start, end)) {
        layer = req.fallback_layer;
    }
    // 3. First free layer below the source layer.
    if (layer < 0) {
        for (int l = req.source_layer + 1;
             l <= req.source_layer + kMaxLayersBelow; ++l) {
            if (layer_is_free(edit, l, start, end)) {
                layer = l;
                break;
            }
        }
    }
    if (layer < 0) {
        outcome.error = manual_steps(req.file, start, req.frame_count);
        log_error(&sdk_logger(),
                  "placement: no free layer found; " + outcome.error);
        return outcome;
    }

    OBJECT_HANDLE object = edit->create_object_from_media_file(
        req.file.wstring().c_str(), layer, start, req.frame_count);
    if (object == nullptr) {
        outcome.error = manual_steps(req.file, start, req.frame_count);
        log_error(&sdk_logger(),
                  format_string("placement: create_object_from_media_file "
                                "failed on layer %d; %s",
                                layer + 1, outcome.error.c_str()));
        return outcome;
    }
    if (!req.object_name.empty()) {
        edit->set_object_name(object, utf8_to_wide(req.object_name).c_str());
    }
    outcome.ok = true;
    outcome.layer = layer;
    log_info(&sdk_logger(),
             format_string("placed %s on layer %d at frame %d (%d frame(s))",
                           req.file.filename().string().c_str(), layer + 1,
                           start, req.frame_count));
    return outcome;
}

bool place_media_files(EDIT_HANDLE* edit,
                       const std::vector<PlaceRequest>& requests,
                       std::vector<PlaceOutcome>& outcomes) {
    outcomes.assign(requests.size(), PlaceOutcome{});
    if (edit == nullptr) {
        for (std::size_t i = 0; i < requests.size(); ++i) {
            outcomes[i].error = "edit handle is not ready";
        }
        return false;
    }
    if (requests.empty()) return true;
    PlaceParam param{&requests, &outcomes};
    if (!edit->call_edit_section_param(&param, &place_callback)) {
        for (std::size_t i = 0; i < requests.size(); ++i) {
            if (!outcomes[i].ok && outcomes[i].error.empty()) {
                outcomes[i].error =
                    "could not enter the edit section (output may be in "
                    "progress); " +
                    manual_steps(requests[i].file, requests[i].start_frame,
                                 requests[i].frame_count);
            }
        }
        log_error(&sdk_logger(),
                  "placement: call_edit_section failed; files were kept");
        return false;
    }
    return true;
}

} // namespace hybrid::plugin
