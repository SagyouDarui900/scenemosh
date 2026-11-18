// cut_detector.cpp — see cut_detector.hpp.

#include "cut_detector.hpp"

#include <algorithm>

namespace hybrid {

std::vector<ObjectSpan> normalize_spans(std::vector<ObjectSpan> spans) {
    std::vector<ObjectSpan> valid;
    valid.reserve(spans.size());
    for (const ObjectSpan& span : spans) {
        if (span.end_frame >= span.start_frame) valid.push_back(span);
    }
    std::sort(valid.begin(), valid.end(),
              [](const ObjectSpan& a, const ObjectSpan& b) {
                  return a.start_frame < b.start_frame ||
                         (a.start_frame == b.start_frame && a.end_frame < b.end_frame);
              });
    std::vector<ObjectSpan> out;
    for (const ObjectSpan& span : valid) {
        if (!out.empty() && span.start_frame <= out.back().end_frame) {
            // Overlap: the later object's start is the boundary.
            out.back().end_frame = span.start_frame - 1;
            if (out.back().end_frame < out.back().start_frame) out.pop_back();
        }
        out.push_back(span);
    }
    return out;
}

std::vector<CutSegment> detect_segments(const std::vector<ObjectSpan>& spans,
                                        std::int64_t before_frames,
                                        std::int64_t after_frames,
                                        std::int64_t scan_start,
                                        std::int64_t scan_end) {
    if (scan_end < scan_start) return {};
    const std::int64_t before = std::max<std::int64_t>(before_frames, 0);
    const std::int64_t after = std::max<std::int64_t>(after_frames, 0);
    const std::vector<ObjectSpan> norm = normalize_spans(spans);

    // One segment per cut: the start of every span except the first.
    // The first span's start and the last span's end are not cuts.
    std::vector<CutSegment> out;
    for (std::size_t i = 1; i < norm.size(); ++i) {
        const std::int64_t cut = norm[i].start_frame;
        if (cut < scan_start || cut > scan_end) continue;
        std::int64_t start = cut - before;
        std::int64_t end = cut + after;
        // Midpoint boundaries with the neighbouring cuts, so segments
        // never overlap. The midpoint is between the two cuts, so the
        // clamp can never push a segment's start past its own cut or
        // its end before its own cut: (prev + cut) / 2 + 1 <= cut and
        // (cut + next) / 2 >= cut for strictly increasing cuts.
        if (i > 1) {
            const std::int64_t prev_cut = norm[i - 1].start_frame;
            start = std::max(start, (prev_cut + cut) / 2 + 1);
        }
        if (i + 1 < norm.size()) {
            const std::int64_t next_cut = norm[i + 1].start_frame;
            end = std::min(end, (cut + next_cut) / 2);
        }
        start = std::max(start, scan_start);
        end = std::min(end, scan_end);
        if (end < start) continue;
        CutSegment seg;
        seg.start_frame = start;
        seg.end_frame = end;
        seg.cut_frame = cut;
        seg.kind = SegmentKind::Cut;
        seg.object_index = static_cast<std::int64_t>(i);
        out.push_back(seg);
    }
    return out;
}

std::vector<CutSegment> detect_object_segments(
    const std::vector<ObjectSpan>& spans, std::int64_t before_frames,
    std::int64_t after_frames, std::int64_t scan_start,
    std::int64_t scan_end) {
    if (scan_end < scan_start) return {};
    const std::int64_t before = std::max<std::int64_t>(before_frames, 0);
    const std::int64_t after = std::max<std::int64_t>(after_frames, 0);
    const std::vector<ObjectSpan> norm = normalize_spans(spans);

    // One segment per object: the object itself plus the lead-in /
    // lead-out, cut at the neighbouring objects' boundaries so
    // segments never overlap. The first segment starts at the scan
    // start at the earliest, the last ends at the scan end at the
    // latest.
    std::vector<CutSegment> out;
    for (std::size_t i = 0; i < norm.size(); ++i) {
        const ObjectSpan& span = norm[i];
        std::int64_t start = span.start_frame - before;
        std::int64_t end = span.end_frame + after;
        // Neighbouring objects' boundaries: the previous object's
        // end + 1 and the next object's start - 1. (Adjacent objects
        // touch, so these are the same frame as the boundary.)
        if (i > 0) {
            start = std::max(start, norm[i - 1].end_frame + 1);
        }
        if (i + 1 < norm.size()) {
            end = std::min(end, norm[i + 1].start_frame - 1);
        }
        start = std::max(start, scan_start);
        end = std::min(end, scan_end);
        if (end < start) continue;
        CutSegment seg;
        seg.start_frame = start;
        seg.end_frame = end;
        seg.cut_frame = span.start_frame;
        seg.kind = SegmentKind::Object;
        seg.object_index = static_cast<std::int64_t>(i);
        out.push_back(seg);
    }
    return out;
}

std::vector<CutSegment> detect_segments_in_mode(
    const std::vector<ObjectSpan>& spans, SegmentMode mode,
    std::int64_t before_frames, std::int64_t after_frames,
    std::int64_t scan_start, std::int64_t scan_end) {
    switch (mode) {
        case SegmentMode::ObjectBased:
            return detect_object_segments(spans, before_frames, after_frames,
                                          scan_start, scan_end);
        case SegmentMode::CutCentred:
        default:
            return detect_segments(spans, before_frames, after_frames,
                                   scan_start, scan_end);
    }
}

} // namespace hybrid
