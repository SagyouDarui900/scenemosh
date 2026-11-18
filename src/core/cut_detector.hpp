// cut_detector.hpp — pure cut-segment math (no SDK dependency).
//
// The plugin side (M3) walks the timeline with EDIT_SECTION::find_object
// and hands the resulting object spans to this module. This module only
// turns spans into glitch segments, in one of two modes (design doc
// section 26, SegmentMode):
//
//   - spans are sorted by start frame; an overlapping span truncates the
//     previous one at its start frame - 1 (design doc section 6.2);
//   - CutCentred: a cut is the boundary between two consecutive spans,
//     i.e. the start frame of every span except the first. There is no
//     cut before the first span or after the last one, so those ends
//     get no segment. Each cut gets one segment [cut - before_frames,
//     cut + after_frames], clamped to [scan_start, scan_end]. When two
//     cuts are close, the boundary between their segments is the
//     midpoint of the two cuts, so segments never overlap and each
//     segment still contains its own cut;
//   - ObjectBased: each span gets one segment [start - before_frames,
//     end + after_frames], clamped to [scan_start, scan_end] and to
//     the neighbouring spans' boundaries, so segments never overlap;
//   - cut_frame is the cut point (the start frame of the object after
//     the cut) for cut segments, and the object's own start frame for
//     object segments. object_index is the 0-based span index the
//     segment refers to.

#pragma once

#include <cstdint>
#include <vector>

namespace hybrid {

// How panel cut detection builds segments (design doc section 26).
enum class SegmentMode {
    CutCentred, // one segment per cut: [cut - before, cut + after]
    ObjectBased // one segment per object: [start - before, end + after]
};

// What a segment refers to: a cut between two objects, or one
// object. The panel list shows "カット C" for the former and
// "オブジェクト N" for the latter.
enum class SegmentKind { Cut, Object };

// One object on the target layer, inclusive frames.
struct ObjectSpan {
    std::int64_t start_frame = 0;
    std::int64_t end_frame = -1;
};

// One glitch segment: around a cut, or covering one object.
struct CutSegment {
    std::int64_t start_frame = 0; // inclusive
    std::int64_t end_frame = -1;  // inclusive
    std::int64_t cut_frame = 0;   // start frame of the object after the cut
    SegmentKind kind = SegmentKind::Cut;
    // 0-based index into the normalized spans: for a cut segment the
    // object after the cut, for an object segment the object itself.
    std::int64_t object_index = -1;
    [[nodiscard]] std::int64_t frame_count() const noexcept {
        return end_frame >= start_frame ? end_frame - start_frame + 1 : 0;
    }
};

// Sort spans, resolve overlaps, drop empty ones. The result is sorted
// and non-overlapping.
std::vector<ObjectSpan> normalize_spans(std::vector<ObjectSpan> spans);

// Build one segment per cut (see the header comment).
// before_frames / after_frames are clamped to >= 0. A cut outside
// [scan_start, scan_end] gets no segment; an empty scan range yields
// none. With before_frames == after_frames == 0 the segment is the
// single cut frame.
std::vector<CutSegment> detect_segments(const std::vector<ObjectSpan>& spans,
                                        std::int64_t before_frames,
                                        std::int64_t after_frames,
                                        std::int64_t scan_start,
                                        std::int64_t scan_end);

// Build one segment per object: [start - before, end + after],
// clamped to [scan_start, scan_end] and to the neighbouring
// objects' boundaries, so segments never overlap. Unlike
// detect_segments, a single object still yields one segment. With
// before_frames == after_frames == 0 the segment is the object
// itself. Spans fully outside the scan range get no segment.
std::vector<CutSegment> detect_object_segments(
    const std::vector<ObjectSpan>& spans, std::int64_t before_frames,
    std::int64_t after_frames, std::int64_t scan_start,
    std::int64_t scan_end);

// Build segments in the requested mode.
std::vector<CutSegment> detect_segments_in_mode(
    const std::vector<ObjectSpan>& spans, SegmentMode mode,
    std::int64_t before_frames, std::int64_t after_frames,
    std::int64_t scan_start, std::int64_t scan_end);

} // namespace hybrid
