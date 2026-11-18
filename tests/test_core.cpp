// test_core.cpp — doctest suite for scenemosh core (M1).
//
// Run: ./build/test_core
// Integration tests (real ffgac/ffedit) only run when the environment
// variable HYBRID_FFGLITCH_DIR points at a directory containing
// ffgac/ffedit (e.g. the Linux FFglitch release). Without it they are
// skipped, and the suite stays hermetic.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "cache_index.hpp"
#include "cut_detector.hpp"
#include "downloader.hpp"
#include "encoder.hpp"
#include "ffglitch.hpp"
#include "file_lock.hpp"
#include "frame_fit.hpp"
#include "generator.hpp"
#include "hash.hpp"
#include "log.hpp"
#include "marker_cache.hpp"
#include "mp4_exporter.hpp"
#include "presets.hpp"
#include "process.hpp"
#include "settings.hpp"
#include "sha256.hpp"

namespace fs = std::filesystem;
using namespace hybrid;

namespace {

// Unique temp dir per test case, removed at the end.
fs::path temp_dir(const std::string& name) {
    fs::path dir = fs::temp_directory_path() / ("hybrid-test-" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

Settings base_settings(const fs::path& cache_dir) {
    Settings s;
    s.uid = "test-obj";
    s.cache_dir = cache_dir;
    s.width = 32;
    s.height = 24;
    s.fps_num = 30;
    s.fps_den = 1;
    s.interval = Interval{0, 29};
    CHECK(s.normalize());
    return s;
}

} // namespace

TEST_CASE("log: VectorLogger records and searches") {
    VectorLogger log;
    log_info(&log, "hello world");
    log_error(&log, "boom happened");
    CHECK(log.entries().size() == 2);
    CHECK(log.has(LogLevel::Info, "hello"));
    CHECK(log.has(LogLevel::Error, "boom"));
    CHECK(!log.has(LogLevel::Warning, "hello"));
    CHECK(log_level_name(LogLevel::Error) == std::string("error"));
    log_info(nullptr, "no crash on null logger");
}

TEST_CASE("settings: normalize clamps and validate reports") {
    Settings s;
    s.uid = "obj-1";
    s.cache_dir = "/tmp/x";
    s.interval = Interval{0, 29};
    s.strength = 250;
    s.crf = 99;
    s.width = 0;
    s.fps_den = 0;
    CHECK(s.normalize());
    CHECK(s.strength == 100);
    CHECK(s.crf == 51);
    CHECK(s.width == 1);
    CHECK(s.fps_den == 1);
    CHECK(s.validate().empty());

    Settings bad;
    bad.uid = "bad uid!"; // space and '!' are unsafe for file names
    bad.cache_dir = "";
    CHECK(!bad.normalize());
    CHECK(!bad.validate().empty());
}

TEST_CASE("settings: preset names and features") {
    CHECK(std::string(preset_display_name(PresetId::Amplify)) == "MV増幅");
    CHECK(std::string(preset_display_name(PresetId::DriftDelta)) == "MV差分ドリフト");
    CHECK(std::string(preset_id_name(PresetId::Datamosh)) == "datamosh");
    CHECK(std::string(preset_feature(PresetId::Amplify)) == "mv");
    CHECK(std::string(preset_feature(PresetId::DriftDelta)) == "mv_delta");
    PresetId p{};
    CHECK(preset_from_id_name("random", p));
    CHECK(p == PresetId::Random);
    CHECK(!preset_from_id_name("nope", p));
    RefMode m{};
    CHECK(ref_mode_from_name("screen", m));
    CHECK(m == RefMode::Screen);
    CHECK(!ref_mode_from_name("nope", m));
    CHECK(Interval{0, 29}.frame_count() == 30);
    CHECK(Interval{5, 4}.empty());
}

TEST_CASE("hash: deterministic, field-sensitive, length-prefixed") {
    Fnv1a64 a, b;
    a.mix_string("hello");
    b.mix_string("hello");
    CHECK(a.value() == b.value());
    CHECK(a.hex().size() == 16);

    // "ab"+"c" must differ from "a"+"bc": strings are length-prefixed.
    Fnv1a64 c, d;
    c.mix_string("ab");
    c.mix_string("c");
    d.mix_string("a");
    d.mix_string("bc");
    CHECK(c.value() != d.value());

    // Field order matters.
    Fnv1a64 e, f;
    e.mix_u64(1);
    e.mix_u64(2);
    f.mix_u64(2);
    f.mix_u64(1);
    CHECK(e.value() != f.value());

    const Settings s = base_settings("/tmp/x");
    const std::string h1 = compute_src_hash(s, "0.10.2");
    CHECK(h1.size() == 16);
    CHECK(compute_src_hash(s, "0.10.2") == h1);
    Settings changed = s;
    changed.strength = 51; // strength is not part of the source hash
    CHECK(compute_src_hash(changed, "0.10.2") == h1);
    changed = s;
    changed.width = 64; // resolution is part of the source hash
    CHECK(compute_src_hash(changed, "0.10.2") != h1);
    CHECK(compute_glitch_hash(s, "0.10.2", 0) != compute_glitch_hash(s, "0.10.2", 1));
    CHECK(compute_glitch_hash(s, "0.10.2", 0) != compute_src_hash(s, "0.10.2"));
}

TEST_CASE("presets: generated JS is deterministic and well-formed") {
    for (int i = 0; i < kPresetCount; ++i) {
        PresetId preset{};
        REQUIRE(preset_from_index(i, preset));
        const std::string js = generate_preset_js(preset, 50, 42);
        CHECK(js.find("glitch_frame") != std::string::npos);
        CHECK(js.find("mulberry32") != std::string::npos);
        CHECK(js.find("walk_mvs") != std::string::npos);
        // Mutation is in place via MV.assign; a plain array must never
        // be assigned to a cell (ffedit rejects it).
        CHECK(js.find("assign(MV(") != std::string::npos);
        CHECK(js.find(preset_id_name(preset)) != std::string::npos);
        // Deterministic.
        CHECK(generate_preset_js(preset, 50, 42) == js);
        // A different seed changes the script (the preamble embeds it).
        CHECK(generate_preset_js(preset, 50, 43) != js);
        // Every preset uses the "mv" feature except DriftDelta.
        if (preset == PresetId::DriftDelta) {
            CHECK(std::string(preset_feature(preset)) == "mv_delta");
        } else {
            CHECK(std::string(preset_feature(preset)) == "mv");
        }
    }
    CHECK(amplify_factor(0) == doctest::Approx(1.0));
    CHECK(amplify_factor(100) == doctest::Approx(5.0));
    CHECK(random_range(0) == 1);
    CHECK(random_range(100) == 11);
    int h1 = 0, v1 = 0, h2 = 0, v2 = 0;
    fixed_vector(42, h1, v1);
    fixed_vector(42, h2, v2);
    CHECK(h1 == h2);
    CHECK(v1 == v2);
    CHECK(h1 >= -4);
    CHECK(h1 <= 4);
    CHECK(v1 >= -4);
    CHECK(v1 <= 4);
    // The fixed vector numbers appear in the JS text.
    const std::string js = generate_preset_js(PresetId::Fixed, 50, 42);
    CHECK(js.find("fh = " + std::to_string(h1)) != std::string::npos);
}

TEST_CASE("presets: M4 strength mappings and per-mode JS") {
    // Index table: the 12 M4 modes follow the 5 M1 presets in order.
    PresetId p{};
    CHECK(preset_from_index(0, p));
    CHECK(p == PresetId::Amplify);
    CHECK(preset_from_index(5, p));
    CHECK(p == PresetId::Zoom);
    CHECK(preset_from_index(16, p));
    CHECK(p == PresetId::Sink);
    CHECK(preset_from_index(17, p));
    CHECK(p == PresetId::Fluid);
    CHECK(preset_from_index(18, p));
    CHECK(p == PresetId::Ripple);
    CHECK(preset_from_index(19, p));
    CHECK(p == PresetId::Gravity);
    CHECK(preset_from_index(20, p));
    CHECK(p == PresetId::Transfer);
    CHECK(!preset_from_index(-1, p));
    CHECK(!preset_from_index(kPresetCount, p));
    CHECK(kPresetCount == 21);

    CHECK(std::string(preset_display_name(PresetId::Zoom)) == "ズーム");
    CHECK(std::string(preset_display_name(PresetId::Sink)) == "シンク");
    CHECK(std::string(preset_display_name(PresetId::Fluid)) == "流体");
    CHECK(std::string(preset_display_name(PresetId::Ripple)) == "波紋");
    CHECK(std::string(preset_display_name(PresetId::Gravity)) == "重力");
    CHECK(std::string(preset_display_name(PresetId::Transfer)) == "モーション転送");
    CHECK(std::string(preset_id_name(PresetId::Shuffle)) == "shuffle");
    CHECK(std::string(preset_id_name(PresetId::Fluid)) == "fluid");
    CHECK(std::string(preset_id_name(PresetId::Ripple)) == "ripple");
    CHECK(std::string(preset_id_name(PresetId::Gravity)) == "gravity");
    CHECK(std::string(preset_id_name(PresetId::Transfer)) == "transfer");
    CHECK(preset_from_id_name("vibrate", p));
    CHECK(p == PresetId::Vibrate);
    CHECK(preset_from_id_name("fluid", p));
    CHECK(p == PresetId::Fluid);
    CHECK(preset_from_id_name("ripple", p));
    CHECK(p == PresetId::Ripple);
    CHECK(preset_from_id_name("gravity", p));
    CHECK(p == PresetId::Gravity);
    CHECK(preset_from_id_name("transfer", p));
    CHECK(p == PresetId::Transfer);
    CHECK(!preset_from_id_name("glide", p)); // not in the first batch

    // Strength mappings (C++ helpers; the JS generator uses them).
    CHECK(zoom_factor(0, 1.0) == doctest::Approx(1.0));
    CHECK(zoom_factor(100, 1.0) == doctest::Approx(2.0));
    CHECK(zoom_factor(100, 0.0) == doctest::Approx(1.0));
    CHECK(zoom_factor(50, 0.5) == doctest::Approx(1.25));
    CHECK(shear_offset(0, 0, 68) == 0);
    CHECK(shear_offset(100, 0, 68) == -17); // (0 - 34) * 0.5
    CHECK(shear_offset(100, 67, 68) == 17); // (67 - 34) * 0.5, rounded
    CHECK(shear_offset(100, 34, 68) == 0);
    CHECK(shift_offset(42, 0) == 0);
    CHECK(shift_offset(42, 50) == 2); // fixed h for seed 42 is 2
    CHECK(shift_offset(42, 100) == 4);
    CHECK(delay_frames(0) == 1);
    CHECK(delay_frames(50) == 15);
    CHECK(delay_frames(100) == 30);
    CHECK(buffer_delay_frames() == 8);
    CHECK(vibrate_amplitude(0) == doctest::Approx(1.0));
    CHECK(vibrate_amplitude(100) == doctest::Approx(8.0));
    CHECK(sink_offset(0) == 0);
    CHECK(sink_offset(50) == 4);
    CHECK(sink_offset(100) == 8);
    CHECK(stop_factor(0) == doctest::Approx(1.0));
    CHECK(stop_factor(50) == doctest::Approx(0.5));
    CHECK(stop_factor(100) == doctest::Approx(0.0));
    CHECK(fluid_window_frames(0) == 2);
    CHECK(fluid_window_frames(50) == 9);
    CHECK(fluid_window_frames(100) == 16);
    CHECK(ripple_amplitude(0) == doctest::Approx(1.0));
    CHECK(ripple_amplitude(100) == doctest::Approx(12.0));
    CHECK(ripple_frequency(0) == doctest::Approx(0.15));
    CHECK(ripple_frequency(100) == doctest::Approx(0.50));
    CHECK(gravity_acceleration(0) == doctest::Approx(0.2));
    CHECK(gravity_acceleration(100) == doctest::Approx(2.0));

    // Each M4 mode's JS carries its own distinctive code.
    const auto has = [](PresetId preset, const std::string& needle) {
        return generate_preset_js(preset, 50, 42).find(needle) !=
               std::string::npos;
    };
    CHECK(has(PresetId::Zoom, "zmax"));
    CHECK(has(PresetId::Shear, "shcy"));
    CHECK(has(PresetId::Shift, "shdh = 2"));
    CHECK(has(PresetId::Slice, "Math.floor(y / 4)"));
    CHECK(has(PresetId::Smear, "saved[0], mv[1]"));
    CHECK(has(PresetId::Vibrate, "Math.sin(vph"));
    CHECK(has(PresetId::Mirror, "rng() * 100 < mistr"));
    CHECK(has(PresetId::Shuffle, "shsnap"));
    CHECK(has(PresetId::Buffer, "mv_queue.length > 8"));
    CHECK(has(PresetId::Delay, "mv_queue.length > 15"));
    CHECK(has(PresetId::Stop, "stf"));
    CHECK(has(PresetId::Sink, "sk_v"));
    CHECK(has(PresetId::Fluid, "mv_queue.length > 9"));
    CHECK(has(PresetId::Fluid, "sum_h / count"));
    CHECK(has(PresetId::Ripple, "rfreq + rphase"));
    CHECK(has(PresetId::Gravity, "gravity_vel = Math.min(gravity_vel +"));
    CHECK(has(PresetId::Gravity, "g_thresh"));
    // The delay queue state exists in every script (harmless when unused).
    CHECK(has(PresetId::Amplify, "var mv_queue = []"));

    // Angle parameter tests for directional presets
    CHECK(preset_supports_angle(PresetId::Fixed));
    CHECK(preset_supports_angle(PresetId::DriftDelta));
    CHECK(preset_supports_angle(PresetId::Shift));
    CHECK(preset_supports_angle(PresetId::Gravity));
    CHECK(preset_supports_angle(PresetId::Sink));
    CHECK(preset_supports_angle(PresetId::Ripple));
    CHECK(preset_supports_angle(PresetId::Transfer));
    CHECK(!preset_supports_angle(PresetId::Amplify));
    CHECK(!preset_supports_angle(PresetId::Zoom));

    // Transfer preset in cut-centred mode:
    // Frames before cut are pushed to mv_queue.
    // Frames at and after cut replay from mv_queue.
    const std::string js_trans_cut = generate_preset_js(PresetId::Transfer, 50, 0, 0, 30, 60);
    CHECK(js_trans_cut.find("var cut = 30;") != std::string::npos);
    CHECK(js_trans_cut.find("mv_queue.push(mvs.dup())") != std::string::npos);
    CHECK(js_trans_cut.find("src_k = (frame_index - cut) % mv_queue.length") != std::string::npos);

    // Fixed preset with 90 degrees angle: vectors should point straight down (fh=0, fv>0)
    const std::string js_fixed_90 = generate_preset_js(PresetId::Fixed, 50, 0, 90);
    CHECK(js_fixed_90.find("var fh = 0;") != std::string::npos);
    CHECK(js_fixed_90.find("var fv = 16;") != std::string::npos);

    // Datamosh preset with cut_offset (cut-centred mode):
    // Before the cut, frames pass through and update saved_grid.
    // At and after the cut, saved_grid is applied.
    const std::string js_mosh_cut = generate_preset_js(PresetId::Datamosh, 50, 0, 0, 30, 60);
    CHECK(js_mosh_cut.find("var cut = 30;") != std::string::npos);
    CHECK(js_mosh_cut.find("frame_index < cut") != std::string::npos);
    CHECK(js_mosh_cut.find("saved_grid = mvs.dup()") != std::string::npos);

    // Other presets with cut_offset: pass through cleanly before cut
    const std::string js_amp_cut = generate_preset_js(PresetId::Amplify, 50, 0, 0, 30, 60);
    CHECK(js_amp_cut.find("var cut = 30;") != std::string::npos);
    CHECK(js_amp_cut.find("frame_index < cut") != std::string::npos);
}

TEST_CASE("cut_detector: cut-centred segments") {
    // One cut (at frame 40) between two objects: one segment
    // [cut - before, cut + after] that spans the cut.
    const auto segs = detect_segments({{10, 29}, {40, 59}}, 5, 5, 0, 100);
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].start_frame == 35);
    CHECK(segs[0].end_frame == 45);
    CHECK(segs[0].cut_frame == 40);
    // No cut before the first span or after the last one.
    CHECK(detect_segments({{10, 20}}, 5, 5, 0, 100).empty());
    CHECK(detect_segments({}, 5, 5, 0, 100).empty());
    CHECK(detect_segments({{10, 20}}, 5, 5, 50, 40).empty()); // bad scan range
    // Negative before/after counts as 0: the segment is the cut
    // frame alone.
    const auto segs0 = detect_segments({{10, 29}, {40, 59}}, -3, -3, 0, 100);
    REQUIRE(segs0.size() == 1);
    CHECK(segs0[0].start_frame == 40);
    CHECK(segs0[0].end_frame == 40);
    CHECK(segs0[0].cut_frame == 40);
}

TEST_CASE("cut_detector: close cuts share the midpoint") {
    // Cuts at 20 and 30 with before=after=30: the boundary between
    // the two segments is the midpoint (20 + 30) / 2 = 25, so neither
    // segment overlaps the other and each still contains its cut.
    const auto segs =
        detect_segments({{0, 9}, {20, 29}, {30, 39}}, 30, 30, 0, 100);
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].start_frame == 0); // clamped to the scan start
    CHECK(segs[0].end_frame == 25);
    CHECK(segs[0].cut_frame == 20);
    CHECK(segs[1].start_frame == 26);
    CHECK(segs[1].end_frame == 60);
    CHECK(segs[1].cut_frame == 30);
    // Well-separated cuts keep their full lead-in / lead-out.
    const auto wide =
        detect_segments({{0, 29}, {40, 59}, {100, 129}}, 30, 30, 0, 200);
    REQUIRE(wide.size() == 2);
    CHECK(wide[0].start_frame == 10);
    CHECK(wide[0].end_frame == 70); // midpoint (40 + 100) / 2
    CHECK(wide[0].cut_frame == 40);
    CHECK(wide[1].start_frame == 71);
    CHECK(wide[1].end_frame == 130);
    CHECK(wide[1].cut_frame == 100);
    // Expansion is clamped to the scan range.
    const auto clipped =
        detect_segments({{0, 49}, {50, 99}}, 30, 30, 20, 80);
    REQUIRE(clipped.size() == 1);
    CHECK(clipped[0].start_frame == 20);
    CHECK(clipped[0].end_frame == 80);
    CHECK(clipped[0].cut_frame == 50);
}

TEST_CASE("cut_detector: object-based segments") {
    // One segment per object: [start - before, end + after].
    const auto segs =
        detect_object_segments({{0, 49}, {50, 185}}, 30, 30, 0, 300);
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].start_frame == 0); // clamped to the scan start
    CHECK(segs[0].end_frame == 49);  // cut at the next object's start - 1
    CHECK(segs[0].kind == SegmentKind::Object);
    CHECK(segs[0].object_index == 0);
    CHECK(segs[0].cut_frame == 0);
    CHECK(segs[1].start_frame == 50);
    CHECK(segs[1].end_frame == 215);
    CHECK(segs[1].kind == SegmentKind::Object);
    CHECK(segs[1].object_index == 1);
    CHECK(segs[1].cut_frame == 50);
    // Unlike cut-centred mode, a single object still yields one
    // segment.
    const auto single = detect_object_segments({{10, 20}}, 5, 5, 0, 100);
    REQUIRE(single.size() == 1);
    CHECK(single[0].start_frame == 5);
    CHECK(single[0].end_frame == 25);
    CHECK(single[0].kind == SegmentKind::Object);
    CHECK(single[0].object_index == 0);
    // Wide lead-in / lead-out is cut at the neighbouring objects'
    // boundaries, so the segments never overlap.
    const auto wide =
        detect_object_segments({{0, 49}, {50, 99}}, 100, 100, 0, 200);
    REQUIRE(wide.size() == 2);
    CHECK(wide[0].start_frame == 0);
    CHECK(wide[0].end_frame == 49);
    CHECK(wide[1].start_frame == 50);
    CHECK(wide[1].end_frame == 199);
    CHECK(wide[0].end_frame < wide[1].start_frame);
    // Negative before/after counts as 0: the segment is the object
    // itself.
    const auto segs0 =
        detect_object_segments({{10, 20}}, -3, -3, 0, 100);
    REQUIRE(segs0.size() == 1);
    CHECK(segs0[0].start_frame == 10);
    CHECK(segs0[0].end_frame == 20);
    // Empty input and a bad scan range yield none; a span fully
    // outside the scan range is dropped.
    CHECK(detect_object_segments({}, 5, 5, 0, 100).empty());
    CHECK(detect_object_segments({{10, 20}}, 5, 5, 50, 40).empty());
    CHECK(detect_object_segments({{10, 20}}, 5, 5, 30, 100).empty());
    // detect_segments_in_mode dispatches to both modes.
    const auto by_cut =
        detect_segments_in_mode({{10, 29}, {40, 59}}, SegmentMode::CutCentred,
                                5, 5, 0, 100);
    REQUIRE(by_cut.size() == 1);
    CHECK(by_cut[0].kind == SegmentKind::Cut);
    const auto by_object =
        detect_segments_in_mode({{10, 29}, {40, 59}}, SegmentMode::ObjectBased,
                                5, 5, 0, 100);
    REQUIRE(by_object.size() == 2);
    CHECK(by_object[0].kind == SegmentKind::Object);
    CHECK(by_object[1].kind == SegmentKind::Object);
}

TEST_CASE("cut_detector: segment kind and object index") {
    // Cut-centred segments carry the kind and the index of the
    // object after the cut.
    const auto segs =
        detect_segments({{0, 29}, {40, 59}, {100, 129}}, 30, 30, 0, 200);
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].kind == SegmentKind::Cut);
    CHECK(segs[0].object_index == 1);
    CHECK(segs[0].cut_frame == 40);
    CHECK(segs[1].kind == SegmentKind::Cut);
    CHECK(segs[1].object_index == 2);
    CHECK(segs[1].cut_frame == 100);
}

TEST_CASE("cut_detector: overlaps, gaps, empty input") {
    // Overlapping spans: the later start truncates the earlier one.
    const auto norm = normalize_spans({{10, 30}, {20, 40}});
    REQUIRE(norm.size() == 2);
    CHECK(norm[0].start_frame == 10);
    CHECK(norm[0].end_frame == 19);
    CHECK(norm[1].start_frame == 20);
    CHECK(norm[1].end_frame == 40);
}

TEST_CASE("frame_fit: same size copies unchanged") {
    // 2x2 source with distinct pixels.
    const std::vector<std::uint8_t> src = {
        10, 20, 30, 255, 40, 50, 60, 255,
        70, 80, 90, 255, 100, 110, 120, 255};
    std::vector<std::uint8_t> dst(src.size(), 0);
    fit_frame_centered(src, 2, 2, dst, 2, 2);
    CHECK(dst == src);
}

TEST_CASE("frame_fit: smaller source is centred with black padding") {
    // 2x2 source into a 4x4 buffer: the source sits at (1, 1)
    // ((4 - 2) / 2), everything else is opaque black.
    const std::vector<std::uint8_t> src = {
        10, 20, 30, 255, 40, 50, 60, 255,
        70, 80, 90, 255, 100, 110, 120, 255};
    std::vector<std::uint8_t> dst(4 * 4 * 4, 0);
    fit_frame_centered(src, 2, 2, dst, 4, 4);
    const auto px = [&dst](int x, int y) {
        const std::size_t i = (static_cast<std::size_t>(y) * 4 + x) * 4;
        return std::array<std::uint8_t, 4>{dst[i], dst[i + 1], dst[i + 2],
                                           dst[i + 3]};
    };
    CHECK(px(0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255});
    CHECK(px(3, 3) == std::array<std::uint8_t, 4>{0, 0, 0, 255});
    CHECK(px(1, 1) == std::array<std::uint8_t, 4>{10, 20, 30, 255});
    CHECK(px(2, 1) == std::array<std::uint8_t, 4>{40, 50, 60, 255});
    CHECK(px(1, 2) == std::array<std::uint8_t, 4>{70, 80, 90, 255});
    CHECK(px(2, 2) == std::array<std::uint8_t, 4>{100, 110, 120, 255});
    CHECK(px(0, 1) == std::array<std::uint8_t, 4>{0, 0, 0, 255});
}

TEST_CASE("frame_fit: larger source is centre-cropped") {
    // 4x4 source (red = x, green = y) into a 2x2 buffer: the centre
    // 2x2, i.e. source pixels (1, 1), (2, 1), (1, 2), (2, 2).
    std::vector<std::uint8_t> src(4 * 4 * 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 4 + x) * 4;
            src[i] = static_cast<std::uint8_t>(x);
            src[i + 1] = static_cast<std::uint8_t>(y);
            src[i + 2] = 0;
            src[i + 3] = 255;
        }
    }
    std::vector<std::uint8_t> dst(2 * 2 * 4, 0);
    fit_frame_centered(src, 4, 4, dst, 2, 2);
    const auto px = [&dst](int x, int y) {
        const std::size_t i = (static_cast<std::size_t>(y) * 2 + x) * 4;
        return std::array<std::uint8_t, 4>{dst[i], dst[i + 1], dst[i + 2],
                                           dst[i + 3]};
    };
    CHECK(px(0, 0) == std::array<std::uint8_t, 4>{1, 1, 0, 255});
    CHECK(px(1, 0) == std::array<std::uint8_t, 4>{2, 1, 0, 255});
    CHECK(px(0, 1) == std::array<std::uint8_t, 4>{1, 2, 0, 255});
    CHECK(px(1, 1) == std::array<std::uint8_t, 4>{2, 2, 0, 255});
}

TEST_CASE("frame_fit: odd size difference and bad input") {
    // 3x3 source into a 4x4 buffer: the source starts at (0, 0)
    // ((4 - 3) / 2 = 0), the last row and column stay black.
    std::vector<std::uint8_t> src(3 * 3 * 4, 200);
    for (std::size_t i = 0; i < src.size(); i += 4) src[i + 3] = 255;
    std::vector<std::uint8_t> dst(4 * 4 * 4, 0);
    fit_frame_centered(src, 3, 3, dst, 4, 4);
    CHECK(dst[0] == 200);              // source pixel (0, 0)
    CHECK(dst[3 * 4] == 0);            // black column (3, 0)
    CHECK(dst[3 * 4 * 4] == 0);        // black row (0, 3)
    CHECK(dst[3 * 4 * 4 + 3] == 255);  // black is opaque
    // Bad input leaves the destination unchanged.
    std::vector<std::uint8_t> small(4, 9);
    std::vector<std::uint8_t> keep(4 * 4 * 4, 7);
    fit_frame_centered(small, 2, 2, keep, 4, 4);
    CHECK(keep == std::vector<std::uint8_t>(4 * 4 * 4, 7));
    fit_frame_centered(src, 3, 3, keep, 0, 4);
    CHECK(keep == std::vector<std::uint8_t>(4 * 4 * 4, 7));
}

TEST_CASE("frame_fit: fit_frame_bilinear scales and preserves aspect") {
    // 1. Same size copies directly.
    const std::vector<std::uint8_t> src = {
        10, 20, 30, 255, 40, 50, 60, 255,
        70, 80, 90, 255, 100, 110, 120, 255};
    std::vector<std::uint8_t> dst(src.size(), 0);
    fit_frame_bilinear(src, 2, 2, dst, 2, 2);
    CHECK(dst == src);

    // 2. Uniform 2x upscale (2x2 -> 4x4, same 1:1 aspect ratio):
    // dst should fill all 4x4 pixels without any black border.
    std::vector<std::uint8_t> dst4(4 * 4 * 4, 0);
    fit_frame_bilinear(src, 2, 2, dst4, 4, 4);
    for (std::size_t i = 0; i < dst4.size(); i += 4) {
        CHECK(dst4[i + 3] == 255); // Alpha is 255
        // Red channel is between 10 and 100
        CHECK(dst4[i] >= 10);
        CHECK(dst4[i] <= 100);
    }
    // Top-left pixel should be close to 10
    CHECK(dst4[0] <= 25);
    // Bottom-right pixel should be close to 100
    CHECK(dst4[(3 * 4 + 3) * 4] >= 85);

    // 3. Aspect mismatch (2x1 into 4x4):
    // 2x1 scaled aspect-fit into 4x4 results in 4x2 centered vertically (off_y = 1).
    // Row 0 and row 3 must be opaque black.
    const std::vector<std::uint8_t> wide_src = {
        100, 150, 200, 255, 100, 150, 200, 255};
    std::vector<std::uint8_t> pillar(4 * 4 * 4, 0);
    fit_frame_bilinear(wide_src, 2, 1, pillar, 4, 4);
    // Row 0 is black
    CHECK(pillar[0] == 0);
    CHECK(pillar[3] == 255);
    CHECK(pillar[3 * 4] == 0);
    // Row 1 (centered image) has color
    CHECK(pillar[1 * 4 * 4] == 100);
    CHECK(pillar[1 * 4 * 4 + 1] == 150);
    // Row 3 is black
    CHECK(pillar[3 * 4 * 4] == 0);
    CHECK(pillar[3 * 4 * 4 + 3] == 255);

    // 4. Bad input leaves destination unchanged.
    std::vector<std::uint8_t> keep(4 * 4 * 4, 42);
    fit_frame_bilinear(wide_src, 0, 1, keep, 4, 4);
    CHECK(keep == std::vector<std::uint8_t>(4 * 4 * 4, 42));
}

TEST_CASE("cache_index: names, part paths, parsing") {
    CHECK(src_filename("u1", "0123456789abcdef") == "u1-0123456789abcdef.src.mp4");
    CHECK(glitch_filename("u1", "0123456789abcdef", "fedcba9876543210", 0) ==
          "u1-0123456789abcdef-fedcba9876543210.mp4");
    CHECK(glitch_filename("u1", "0123456789abcdef", "fedcba9876543210", 3) ==
          "u1-0123456789abcdef-fedcba9876543210-g3.mp4");
    CHECK(export_filename("u1", "0123456789abcdef", "fedcba9876543210") ==
          "u1-0123456789abcdef-fedcba9876543210.export.mp4");

    const fs::path final_path = fs::path("/tmp/cache") / "u1-0123456789abcdef.mp4";
    CHECK(part_path(final_path).filename().string() == "u1-0123456789abcdef.part.mp4");
    CHECK(strip_part(part_path(final_path)) == final_path);

    const auto entry = parse_cache_filename(
        fs::path("/tmp/cache") / "u1-0123456789abcdef-fedcba9876543210-g2.mp4");
    REQUIRE(entry.has_value());
    CHECK(entry->uid == "u1");
    CHECK(entry->generation == 2);
    CHECK(!entry->is_src);
    const auto src_entry = parse_cache_filename(
        fs::path("/tmp/cache") / "u1-0123456789abcdef.src.mp4");
    REQUIRE(src_entry.has_value());
    CHECK(src_entry->is_src);
    CHECK(src_entry->src_hash == "0123456789abcdef");
    const auto exp_entry = parse_cache_filename(
        fs::path("/tmp/cache") / "u1-0123456789abcdef-fedcba9876543210.export.mp4");
    REQUIRE(exp_entry.has_value());
    CHECK(exp_entry->is_export);
    CHECK(exp_entry->glitch_hash == "fedcba9876543210");
    CHECK(!parse_cache_filename(fs::path("/tmp/cache") / "random.mp4").has_value());
    CHECK(!parse_cache_filename(fs::path("/tmp/cache") / "u1-nothex.mp4").has_value());
}

TEST_CASE("cache_index: list and orphans in a real directory") {
    const fs::path dir = temp_dir("cache");
    const CacheIndex index(dir);
    // Write three files: one live src, one glitch for it, one dead uid.
    auto write = [&](const std::string& name) {
        std::ofstream out(dir / name, std::ios::binary);
        out << "data";
    };
    write("live-0123456789abcdef.src.mp4");
    write("live-0123456789abcdef-fedcba9876543210.mp4");
    write("dead-0123456789abcdef.src.mp4");
    CHECK(index.list().size() == 3);
    CHECK(index.total_size_bytes() == 12);
    const auto orphans = index.orphans({"live"});
    REQUIRE(orphans.size() == 1);
    CHECK(orphans[0].uid == "dead");
    CHECK(index.remove_orphans({"live"}) == 1);
    CHECK(index.list().size() == 2);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("file_lock: missing file can be replaced, writable file too") {
    const fs::path dir = temp_dir("lock");
    CHECK(can_replace(dir / "missing.mp4"));
    const fs::path f = dir / "present.mp4";
    { std::ofstream out(f); out << "x"; }
    CHECK(can_replace(f));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("sha256: known vectors") {
    CHECK(sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const fs::path dir = temp_dir("sha");
    const fs::path f = dir / "abc.txt";
    { std::ofstream out(f); out << "abc"; }
    CHECK(sha256_file_hex(f) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_file_hex(dir / "missing") == "");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("ffglitch: argument building and version parsing") {
    const auto args = build_ffgac_args(1920, 1080, 30000, 1001, "out.mp4");
    const std::string joined = [&] {
        std::string s;
        for (const auto& a : args) s += a + " ";
        return s;
    }();
    CHECK(joined.find("-pix_fmt rgba") != std::string::npos);
    CHECK(joined.find("-s 1920x1080") != std::string::npos);
    CHECK(joined.find("-framerate 30000/1001") != std::string::npos);
    CHECK(joined.find("-vcodec mpeg4") != std::string::npos);
    CHECK(joined.find("-mpv_flags +forcemv+nopimb") != std::string::npos);
    CHECK(joined.find("-bf 0") != std::string::npos);

    const auto eargs = build_ffedit_args("mv", "s.js", "in.mp4", "out.mp4");
    CHECK(eargs[0] == "-i");
    CHECK(eargs[3] == "mv");

    CHECK(parse_tool_version("ffedit version ffglitch-0.10.2 Copyright (c) 2000-2024") ==
          std::optional<std::string>("0.10.2"));
    CHECK(parse_tool_version("ffedit version 0.10.2 Copyright (c) 2018-2024 Ramiro Polla") ==
          std::optional<std::string>("0.10.2"));
    CHECK(!parse_tool_version("no version here").has_value());
    CHECK(!find_ffglitch("/nonexistent-dir-xyz").has_value());

    const FFglitchRelease& rel = expected_ffglitch_release();
    CHECK(rel.version == "0.10.2");
    CHECK(rel.url == "https://ffglitch.org/pub/bin/win64/ffglitch-0.10.2-windows-x86_64.zip");
    CHECK(rel.size_bytes == 40311219);
    CHECK(rel.sha256 == "e5190462eafe9c28428d39f8392a6be93dfd6ac2d59ce08848edec79acf965ca");
}

TEST_CASE("downloader: verification rejects wrong size/hash") {
    const fs::path dir = temp_dir("dl");
    const fs::path f = dir / "fake.zip";
    { std::ofstream out(f); out << "not a zip"; }
    const FFglitchRelease rel;
    CHECK(!verify_release_archive(f, rel)); // wrong size
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#ifndef _WIN32
TEST_CASE("process: echo, stdin round-trip, missing executable") {
    const ProcessResult echo = run_capture("/bin/echo", {"hello"}, std::nullopt, 5000);
    CHECK(echo.started);
    CHECK(echo.exit_code == 0);
    CHECK(echo.stdout_text == "hello\n");

    const std::string input = "abc123";
    const ProcessResult cat = run_capture(
        "/bin/cat", {},
        std::optional<std::span<const std::uint8_t>>(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(input.data()), input.size())),
        5000);
    CHECK(cat.started);
    CHECK(cat.exit_code == 0);
    CHECK(cat.stdout_text == input);

    const ProcessResult missing = run_capture("/nonexistent/tool-xyz", {}, std::nullopt, 5000);
    CHECK(!missing.started);
    CHECK(!missing.error.empty());
}

TEST_CASE("mp4_exporter: ffmpeg args") {
    const auto args = build_ffmpeg_args("in.mp4", "out.mp4", "fast", 18);
    std::string joined;
    for (const auto& a : args) joined += a + " ";
    CHECK(joined.find("-c:v libx264") != std::string::npos);
    CHECK(joined.find("-preset fast") != std::string::npos);
    CHECK(joined.find("-crf 18") != std::string::npos);
    std::string err;
    CHECK(!export_mp4("in.mp4", "out.mp4", base_settings("/tmp/x"),
                      "/nonexistent/ffmpeg", nullptr, err));
    CHECK(!err.empty());
}
#endif

TEST_CASE("marker_cache: reused while size is unchanged (F2)") {
    plugin::MarkerCache<int> cache;
    int calls = 0;
    auto generate = [&](int w, int h) {
        ++calls;
        return std::vector<int>(static_cast<std::size_t>(w) * h, w * 100 + h);
    };
    auto a = cache.get(64, 48, generate);
    auto b = cache.get(64, 48, generate);
    CHECK(calls == 1);
    CHECK(a == b); // same shared buffer
    CHECK(a->size() == 64u * 48u);
    CHECK((*a)[0] == 64 * 100 + 48);
    auto c = cache.get(32, 24, generate);
    CHECK(calls == 2);
    CHECK(c != a);
    auto d = cache.get(64, 48, generate);
    CHECK(calls == 3); // size changed back: rebuilt
    CHECK(d != c);
    // Concurrent access on one size: no extra generation, no crash.
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<const std::vector<int>>> results(4);
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i] {
            for (int j = 0; j < 100; ++j) results[i] = cache.get(64, 48, generate);
        });
    }
    for (auto& t : threads) t.join();
    CHECK(calls == 3);
    for (const auto& r : results) CHECK(r == d);
}

// ---- Integration tests with real ffgac/ffedit (Linux only) ----

namespace {

std::optional<FFglitchPaths> integration_tools() {
    const char* dir = std::getenv("HYBRID_FFGLITCH_DIR");
    if (dir == nullptr || dir[0] == '\0') return std::nullopt;
    return find_ffglitch(dir);
}

// Frames with a shifting checkerboard, so motion vectors exist
// across the whole grid (every band and row), and every mode has
// vectors to work on. The pattern shifts 1px right and 2px down
// per frame, wrapping every 16px (the checker cell period).
class FakeProvider final : public IFrameProvider {
public:
    FakeProvider(int w, int h, std::int64_t frames) : w_(w), h_(h), frames_(frames) {}
    std::int64_t frame_count() const override { return frames_; }
    bool read_frame(std::int64_t index, std::span<std::uint8_t> rgba) override {
        if (rgba.size() != static_cast<std::size_t>(w_ * h_ * 4)) return false;
        const int dx = static_cast<int>(index % 16);
        const int dy = static_cast<int>((index * 2) % 16);
        for (int y = 0; y < h_; ++y) {
            for (int x = 0; x < w_; ++x) {
                const int cell = (((x - dx) / 8) + ((y - dy) / 8)) & 1;
                const std::uint8_t v = cell ? 255 : 0;
                const std::size_t off = (static_cast<std::size_t>(y) * w_ +
                                         static_cast<std::size_t>(x)) * 4;
                rgba[off] = v; rgba[off + 1] = v; rgba[off + 2] = v; rgba[off + 3] = 255;
            }
        }
        return true;
    }

private:
    int w_, h_;
    std::int64_t frames_;
};

// Reports a real frame size smaller than the job's scene size and
// serves frames at that size. If the Generator ignored the probe,
// read_frame would get a 128x96 buffer and fail (design doc
// section 19).
class ProbeProvider final : public IFrameProvider {
public:
    ProbeProvider() = default;
    std::int64_t frame_count() const override { return 10; }
    bool probe_frame_size(int& width, int& height) override {
        width = 64;
        height = 48;
        return true;
    }
    bool read_frame(std::int64_t index, std::span<std::uint8_t> rgba) override {
        if (rgba.size() != static_cast<std::size_t>(64 * 48 * 4)) return false;
        const int dx = static_cast<int>(index % 16);
        const int dy = static_cast<int>((index * 2) % 16);
        for (int y = 0; y < 48; ++y) {
            for (int x = 0; x < 64; ++x) {
                const int cell = (((x - dx) / 8) + ((y - dy) / 8)) & 1;
                const std::uint8_t v = cell ? 255 : 0;
                const std::size_t off = (static_cast<std::size_t>(y) * 64 +
                                         static_cast<std::size_t>(x)) * 4;
                rgba[off] = v; rgba[off + 1] = v; rgba[off + 2] = v; rgba[off + 3] = 255;
            }
        }
        return true;
    }
};

} // namespace

TEST_CASE("encoder: probe_frame_size defaults to false") {
    FakeProvider provider(128, 96, 30);
    int w = 0;
    int h = 0;
    CHECK(!provider.probe_frame_size(w, h));
}

#ifndef _WIN32
TEST_CASE("generator: tool versions are detected once per tool paths (F1)") {
    // Fake ffgac/ffedit: shell scripts that count each invocation
    // with no arguments (that is how detect_tool_version runs them)
    // and print a parseable banner, then drain stdin and exit 0.
    // The encode still fails (no output file), but version detection
    // runs before it, which is what this test counts. The ffgac
    // script is also run by the encode step (with arguments); those
    // runs are not counted.
    const fs::path dir = temp_dir("f1");
    const fs::path counter = dir / "count.txt";
    auto write_tool = [&](const std::string& name) {
        const fs::path script = dir / name;
        std::ofstream out(script);
        out << "#!/bin/sh\n";
        out << "if [ \"$#\" -eq 0 ]; then echo x >> '" << counter.string()
            << "'; fi\n";
        out << "echo \"" << name << " version 0.10.2 Copyright\" >&2\n";
        out << "cat > /dev/null\n";
        out << "exit 0\n";
        std::error_code ec;
        fs::permissions(script,
                        fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::owner_exec,
                        ec);
    };
    write_tool("ffgac");
    write_tool("ffedit");
    const FFglitchPaths tools{dir, dir / "ffgac", dir / "ffedit"};

    const fs::path cache = temp_dir("f1cache");
    Settings s = base_settings(cache);
    s.width = 32;
    s.height = 24;
    auto provider = std::make_shared<FakeProvider>(32, 24, 30);
    VectorLogger log;
    Generator gen(tools, &log);
    for (int i = 0; i < 2; ++i) {
        Job job;
        job.settings = s;
        job.provider = provider;
        gen.submit(std::move(job));
    }
    gen.wait_idle();

    // Two jobs, but each tool was started once: 2 lines, not 4.
    std::ifstream in(counter);
    const int lines = static_cast<int>(std::count(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>(), '\n'));
    CHECK(lines == 2);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(cache, ec);
}
#endif

TEST_CASE("integration: encode + ffedit for every preset") {
    const auto tools = integration_tools();
    if (!tools.has_value()) {
        MESSAGE("skipped: HYBRID_FFGLITCH_DIR is not set");
        return;
    }
    const fs::path dir = temp_dir("enc");
    Settings s = base_settings(dir);
    s.width = 128; // 8x6 macroblock grid: bands and rows all exercised
    s.height = 96;
    FakeProvider provider(128, 96, 30);
    VectorLogger log;

    const fs::path src_part = dir / "src.part.mp4";
    EncodeResult enc = encode_source(provider, s, *tools, src_part, &log);
    REQUIRE(enc.ok);
    CHECK(enc.frames_written == 30);
    CHECK(fs::file_size(src_part) > 0);
    const std::string src_hash = sha256_file_hex(src_part);
    CHECK(!src_hash.empty());

    for (int i = 0; i < kPresetCount; ++i) {
        PresetId preset{};
        REQUIRE(preset_from_index(i, preset));
        Settings ps = s;
        ps.preset = preset;
        ps.strength = 50;
        ps.seed = 42;
        const fs::path out_part = dir / (std::string(preset_id_name(preset)) + ".part.mp4");
        EncodeResult app = apply_glitch(src_part, ps, *tools, out_part, &log);
        CHECK_MESSAGE(app.ok, preset_id_name(preset), ": ", app.error);
        if (!app.ok) continue;
        CHECK(fs::file_size(out_part) > 0);
        // Every mode must actually change the output.
        CHECK_MESSAGE(sha256_file_hex(out_part) != src_hash,
                      preset_id_name(preset), ": output is identical to input");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("integration: Generator end-to-end and cache reuse") {
    const auto tools = integration_tools();
    if (!tools.has_value()) {
        MESSAGE("skipped: HYBRID_FFGLITCH_DIR is not set");
        return;
    }
    const fs::path dir = temp_dir("gen");
    Settings s = base_settings(dir);
    s.width = 128;
    s.height = 96;
    auto provider = std::make_shared<FakeProvider>(128, 96, 30);
    VectorLogger log;
    Generator gen(*tools, &log);

    JobResult first;
    Job job;
    job.settings = s;
    job.provider = provider;
    job.ffmpeg_exe = "/usr/bin/ffmpeg";
    job.finished = [&](const JobResult& r) { first = r; };
    gen.submit(std::move(job));
    gen.wait_idle();
    CHECK(first.ok);
    CHECK(!first.glitch_path.empty());
    CHECK(fs::is_regular_file(first.glitch_path));
    CHECK(!first.src_path.empty());
    CHECK(fs::is_regular_file(first.src_path));

    // Same settings again: the finished glitch file is reused, not rebuilt.
    JobResult second;
    Job job2;
    job2.settings = s;
    job2.provider = provider;
    job2.finished = [&](const JobResult& r) { second = r; };
    gen.submit(std::move(job2));
    gen.wait_idle();
    CHECK(second.ok);
    CHECK(second.glitch_path == first.glitch_path);

    // Export step with the system ffmpeg, through an M4 mode (Zoom).
    Settings es = s;
    es.export_mp4 = true;
    es.preset = PresetId::Zoom;
    es.seed = 7;
    JobResult third;
    Job job3;
    job3.settings = es;
    job3.provider = provider;
    job3.ffmpeg_exe = "/usr/bin/ffmpeg";
    job3.finished = [&](const JobResult& r) { third = r; };
    gen.submit(std::move(job3));
    gen.wait_idle();
    CHECK_MESSAGE(third.ok, third.error);
    CHECK(!third.export_path.empty());
    if (!third.export_path.empty()) CHECK(fs::is_regular_file(third.export_path));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("integration: Generator cancel stops a running job and drops queued jobs") {
    const auto tools = integration_tools();
    if (!tools.has_value()) {
        MESSAGE("skipped: HYBRID_FFGLITCH_DIR is not set");
        return;
    }
    const fs::path dir = temp_dir("cancel");
    Settings s = base_settings(dir);
    s.width = 64;
    s.height = 48;
    s.interval = Interval{0, 599}; // 600 frames: the encode runs long enough to cancel
    auto provider = std::make_shared<FakeProvider>(64, 48, 600);
    VectorLogger log;
    Generator gen(*tools, &log);

    std::atomic<bool> started{false};
    JobResult first;
    JobResult second;
    Job job1;
    job1.settings = s;
    job1.provider = provider;
    job1.progress = [&](double, const std::string&) { started.store(true); };
    job1.finished = [&](const JobResult& r) { first = r; };
    gen.submit(std::move(job1));

    // A second job, queued behind the first.
    Settings s2 = s;
    s2.uid = "test-obj-2";
    Job job2;
    job2.settings = s2;
    job2.provider = provider;
    job2.finished = [&](const JobResult& r) { second = r; };
    gen.submit(std::move(job2));

    // Wait until the first job is provably encoding, then cancel.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!started.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(started.load());
    gen.cancel();
    gen.wait_idle();

    CHECK(!first.ok);
    CHECK(first.cancelled);
    CHECK(!second.ok);
    CHECK(second.cancelled);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("integration: Generator uses the probed frame size") {
    const auto tools = integration_tools();
    if (!tools.has_value()) {
        MESSAGE("skipped: HYBRID_FFGLITCH_DIR is not set");
        return;
    }
    const fs::path dir = temp_dir("probe");
    Settings s = base_settings(dir);
    s.width = 128; // scene size; the provider renders 64x48
    s.height = 96;
    auto provider = std::make_shared<ProbeProvider>();
    VectorLogger log;
    Generator gen(*tools, &log);
    JobResult result;
    Job job;
    job.settings = s;
    job.provider = provider;
    job.finished = [&](const JobResult& r) { result = r; };
    gen.submit(std::move(job));
    gen.wait_idle();
    CHECK_MESSAGE(result.ok, result.error);
    CHECK(fs::is_regular_file(result.src_path));
    CHECK(fs::is_regular_file(result.glitch_path));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
