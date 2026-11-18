// presets.cpp — see presets.hpp.

#include "presets.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace hybrid {

void fixed_vector(std::uint32_t seed, int& out_h, int& out_v) noexcept {
    // Deterministic values in [-4, 4] taken from the seed.
    const std::uint32_t s = seed;
    out_h = static_cast<int>(s % 9) - 4;
    out_v = static_cast<int>((s / 9) % 9) - 4;
}

double amplify_factor(int strength) noexcept {
    const int s = std::clamp(strength, 0, 100);
    return 1.0 + static_cast<double>(s) / 25.0;
}

int random_range(int strength) noexcept {
    const int s = std::clamp(strength, 0, 100);
    return 1 + s / 10;
}

double zoom_factor(int strength, double dist_ratio) noexcept {
    const int s = std::clamp(strength, 0, 100);
    const double r = std::clamp(dist_ratio, 0.0, 1.0);
    return 1.0 + (static_cast<double>(s) / 100.0) * r;
}

int shear_offset(int strength, int y, int rows) noexcept {
    const int s = std::clamp(strength, 0, 100);
    if (rows <= 0) return 0;
    const double cy = static_cast<double>(rows) / 2.0;
    return static_cast<int>(std::lround((static_cast<double>(y) - cy) *
                                         (static_cast<double>(s) / 100.0) *
                                         0.5));
}

int shift_offset(std::uint32_t seed, int strength) noexcept {
    const int s = std::clamp(strength, 0, 100);
    int fh = 0, fv = 0;
    fixed_vector(seed, fh, fv);
    return static_cast<int>(std::lround(static_cast<double>(fh) *
                                         (static_cast<double>(s) / 100.0) *
                                         2.0));
}

int delay_frames(int strength) noexcept {
    const int s = std::clamp(strength, 0, 100);
    return 1 + (s * 29) / 100;
}

int buffer_delay_frames() noexcept { return 8; }

double vibrate_amplitude(int strength) noexcept {
    const int s = std::clamp(strength, 0, 100);
    return 1.0 + static_cast<double>(s) * 7.0 / 100.0;
}

int sink_offset(int strength) noexcept {
    const int s = std::clamp(strength, 0, 100);
    return static_cast<int>(std::lround(static_cast<double>(s) * 8.0 / 100.0));
}

double stop_factor(int strength) noexcept {
    const int s = std::clamp(strength, 0, 100);
    return 1.0 - static_cast<double>(s) / 100.0;
}

int fluid_window_frames(int strength) noexcept {
    const int s = std::clamp(strength, 0, 100);
    return 2 + (s * 14) / 100;
}

double ripple_amplitude(int strength) noexcept {
    const int s = std::clamp(strength, 0, 100);
    return 1.0 + static_cast<double>(s) * 11.0 / 100.0;
}

double ripple_frequency(int strength) noexcept {
    const int s = std::clamp(strength, 0, 100);
    return 0.15 + static_cast<double>(s) * 0.35 / 100.0;
}

double gravity_acceleration(int strength) noexcept {
    const int s = std::clamp(strength, 0, 100);
    return 0.2 + static_cast<double>(s) * 1.8 / 100.0;
}

namespace {

// Format a double with exactly 3 decimals (e.g. 3.000) so the JS text
// is stable across platforms.
std::string fmt3(double v) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    out << v;
    return out.str();
}

std::string common_preamble(PresetId preset, int strength, std::uint32_t seed) {
    std::ostringstream js;
    js << "\"use strict\";\n";
    js << "// scenemosh preset: " << preset_id_name(preset)
       << " strength=" << std::clamp(strength, 0, 100)
       << " seed=" << seed << "\n";
    js << "function mulberry32(a){return function(){a|=0;a=a+0x6D2B79F5|0;"
          "var t=Math.imul(a^a>>>15,1|a);"
          "t=t+Math.imul(t^t>>>7,61|t)^t;"
          "return((t^t>>>14)>>>0)/4294967296;};}\n";
    js << "var rng = mulberry32(" << seed << ");\n";
    js << "var frame_index = 0;\n";
    js << "var saved_grid = null;\n";
    js << "var drift_h = 0;\n";
    js << "var drift_v = 0;\n";
    js << "var gravity_vel = 0;\n";
    js << "var mv_queue = [];\n"; // Buffer / Delay / Fluid: past grids, oldest first
    // Walk every motion vector. fn(mv, y, x, i) receives the MV object
    // (readable as mv[0]/mv[1]) and returns [h, v] or null to keep it.
    // i is the sub-block index (0 for a plain cell).
    //
    // IMPORTANT (verified against real ffedit 0.10.2): a cell must never
    // be replaced with a plain array — "MVArrays/MVPtrs can only be
    // assigned 'null' or MV(x,y)". Mutation is always in place, via
    // mv.assign(MV(h, v)). Null cells are skipped so no vector is
    // created where the encoder wrote none.
    js << "function walk_mvs(mvs, fn){\n"
          "  mvs.forEach(function (mv, y, x) {\n"
          "    if (mv === null || mv === undefined) return;\n"
          "    if (Array.isArray(mv)) {\n"
          "      for (var i = 0; i < mv.length; i++) {\n"
          "        var sub = mv[i];\n"
          "        if (sub === null || sub === undefined) continue;\n"
          "        var r = fn(sub, y, x, i);\n"
          "        if (r) sub.assign(MV(r[0], r[1]));\n"
          "      }\n"
          "      return;\n"
          "    }\n"
          "    var r2 = fn(mv, y, x, 0);\n"
          "    if (r2) mv.assign(MV(r2[0], r2[1]));\n"
          "  });\n"
          "}\n";
    js << "function grid_at(grid, y, x, i){\n"
          "  if (!grid || y >= grid.length) return null;\n"
          "  var row = grid[y];\n"
          "  if (!row || x >= row.length) return null;\n"
          "  var cell = row[x];\n"
          "  if (cell === null || cell === undefined) return null;\n"
          "  if (Array.isArray(cell)) return (i < cell.length) ? cell[i] : null;\n"
          "  return (i === 0) ? cell : null;\n"
          "}\n";
    return js.str();
}

std::string glitch_frame_open() {
    return std::string(
        "export function glitch_frame(frame, stream) {\n"
        "  var feat = frame.mv ? frame.mv : frame.mv_delta;\n"
        "  if (!feat || !feat.forward) { frame_index++; return; }\n"
        "  feat.overflow = \"truncate\";\n"
        "  var mvs = feat.forward;\n");
}

std::string glitch_frame_close() {
    return "  frame_index++;\n}\n";
}

} // namespace

std::string generate_preset_js(PresetId preset, int strength, std::uint32_t seed,
                               int angle, std::int64_t cut_offset,
                               std::int64_t total_frames) {
    const int s = std::clamp(strength, 0, 100);
    const int a = (angle % 360 + 360) % 360;
    const double rad = static_cast<double>(a) * (3.14159265358979323846 / 180.0);
    const double cos_a = std::cos(rad);
    const double sin_a = std::sin(rad);

    std::ostringstream js;
    js << common_preamble(preset, s, seed);
    js << glitch_frame_open();

    if (cut_offset >= 0 && preset != PresetId::Datamosh &&
        preset != PresetId::Fluid && preset != PresetId::Buffer &&
        preset != PresetId::Delay && preset != PresetId::Transfer) {
        // In cut-centred mode, keep frames before the cut point clean
        // so the transition starts cleanly at the cut point.
        js << "  var cut = " << cut_offset << ";\n"
              "  if (frame_index < cut) { frame_index++; return; }\n";
    }

    switch (preset) {
    case PresetId::Amplify: {
        // Scale every vector by 1.0 (strength 0) .. 5.0 (strength 100).
        js << "  var f = " << fmt3(amplify_factor(s)) << ";\n";
        js << "  walk_mvs(mvs, function (mv) {\n"
              "    return [Math.round(mv[0] * f), Math.round(mv[1] * f)];\n"
              "  });\n";
        break;
    }
    case PresetId::Fixed: {
        int fh = 0, fv = 0;
        if (a == 0 && seed != 0) {
            fixed_vector(seed, fh, fv);
        } else {
            const double mag = (static_cast<double>(s) * 24.0 / 100.0);
            fh = static_cast<int>(std::round(mag * cos_a));
            fv = static_cast<int>(std::round(mag * sin_a));
        }
        js << "  var fh = " << fh << ";\n";
        js << "  var fv = " << fv << ";\n";
        js << "  walk_mvs(mvs, function (mv) { return [fh, fv]; });\n";
        break;
    }
    case PresetId::Random: {
        // Replace every vector with a random one in [-range, range].
        js << "  var range = " << random_range(s) << ";\n";
        js << "  walk_mvs(mvs, function (mv) {\n"
              "    return [Math.floor(rng() * (2 * range + 1)) - range,\n"
              "            Math.floor(rng() * (2 * range + 1)) - range];\n"
              "  });\n";
        break;
    }
    case PresetId::Datamosh: {
        // Classic datamosh:
        // In cut-centred mode (cut_offset > 0), pass through all frames BEFORE
        // the cut so the lead-in is clean and unglitched, while updating saved_grid.
        // At the cut (frame_index >= cut_offset), reuse the cut-point motion vectors
        // so the previous scene smears forward into the next scene.
        // When cut_offset <= 0 (object mode or cut at very start), hold based on strength.
        if (cut_offset > 0) {
            js << "  var cut = " << cut_offset << ";\n"
                  "  if (frame_index < cut) {\n"
                  "    saved_grid = mvs.dup();\n"
                  "  } else {\n"
                  "    if (saved_grid) {\n"
                  "      walk_mvs(mvs, function (mv, y, x, i) {\n"
                  "        var saved = grid_at(saved_grid, y, x, i);\n"
                  "        return saved ? [saved[0], saved[1]] : null;\n"
                  "      });\n"
                  "    }\n"
                  "  }\n";
        } else {
            const int hold = (total_frames > 0)
                ? std::max<int>(1, static_cast<int>(std::round(total_frames * (1.0 - static_cast<double>(s) / 100.0))))
                : std::max<int>(1, (100 - s) / 3);
            js << "  var hold = " << hold << ";\n"
                  "  if (frame_index <= hold) {\n"
                  "    saved_grid = mvs.dup();\n"
                  "  } else {\n"
                  "    if (saved_grid) {\n"
                  "      walk_mvs(mvs, function (mv, y, x, i) {\n"
                  "        var saved = grid_at(saved_grid, y, x, i);\n"
                  "        return saved ? [saved[0], saved[1]] : null;\n"
                  "      });\n"
                  "    }\n"
                  "  }\n";
        }
        break;
    }
    case PresetId::DriftDelta: {
        // mv_delta only: add a small per-frame drift that accumulates,
        // so motion slowly slides in one direction.
        int dh = 0, dv = 0;
        if (a == 0 && seed != 0) {
            fixed_vector(seed, dh, dv);
        } else {
            dh = static_cast<int>(std::round(16.0 * cos_a));
            dv = static_cast<int>(std::round(16.0 * sin_a));
        }
        const double step = 0.05 + static_cast<double>(s) / 100.0 * 0.45;
        js << "  drift_h += (" << dh << " * " << fmt3(step) << ");\n";
        js << "  drift_v += (" << dv << " * " << fmt3(step) << ");\n";
        js << "  walk_mvs(mvs, function (mv) {\n"
              "    return [mv[0] + Math.round(drift_h), mv[1] + Math.round(drift_v)];\n"
              "  });\n";
        break;
    }
    case PresetId::Zoom: {
        // Scale each vector by a factor that grows with the cell's
        // distance from the frame centre: 1.0 at the centre, and
        // 1.0 + strength/100 at the farthest corner (2.0 at 100).
        js << "  var zrows = mvs.length;\n"
              "  var zcols = (zrows > 0 && mvs[0]) ? mvs[0].length : 0;\n"
              "  var zcx = zcols / 2, zcy = zrows / 2;\n"
              "  var zmax = Math.sqrt(zcx * zcx + zcy * zcy) || 1;\n"
              "  var zstr = " << fmt3(static_cast<double>(s) / 100.0) << ";\n";
        js << "  walk_mvs(mvs, function (mv, y, x) {\n"
              "    var zdx = x - zcx, zdy = y - zcy;\n"
              "    var zd = Math.sqrt(zdx * zdx + zdy * zdy) / zmax;\n"
              "    var zf = 1 + zstr * zd;\n"
              "    return [Math.round(mv[0] * zf), Math.round(mv[1] * zf)];\n"
              "  });\n";
        break;
    }
    case PresetId::Shear: {
        // Add a horizontal offset proportional to the row: rows above
        // the centre shift one way, rows below shift the other way.
        js << "  var shrows = mvs.length;\n"
              "  var shcy = shrows / 2;\n"
              "  var shk = " << fmt3(static_cast<double>(s) / 100.0 * 0.5) << ";\n";
        js << "  walk_mvs(mvs, function (mv, y) {\n"
              "    return [mv[0] + Math.round((y - shcy) * shk), mv[1]];\n"
              "  });\n";
        break;
    }
    case PresetId::Shift: {
        // Constant offset for the whole frame along the angle.
        const int sh = (a == 0 && seed != 0) ? shift_offset(seed, s)
                                              : (s * 8 / 100);
        const int sh_h = (a == 0 && seed != 0) ? sh : static_cast<int>(std::round(sh * cos_a));
        const int sh_v = (a == 0 && seed != 0) ? 0 : static_cast<int>(std::round(sh * sin_a));
        js << "  var shdh = " << sh_h << ";\n"
              "  var shdv = " << sh_v << ";\n"
              "  walk_mvs(mvs, function (mv) {\n"
              "    return [mv[0] + shdh, mv[1] + shdv];\n"
              "  });\n";
        break;
    }
    case PresetId::Slice: {
        // Flip the horizontal sign in every second horizontal band
        // (4 rows per band). Strength blends the flip in: at 0 the
        // band is unchanged, at 100 its horizontal sign is fully
        // reversed.
        js << "  var slt = " << fmt3(static_cast<double>(s) / 100.0) << ";\n";
        js << "  walk_mvs(mvs, function (mv, y) {\n"
              "    if (Math.floor(y / 4) % 2 === 1) {\n"
              "      return [Math.round(mv[0] * (1 - 2 * slt)), mv[1]];\n"
              "    }\n"
              "    return null;\n"
              "  });\n";
        break;
    }
    case PresetId::Smear: {
        // Horizontal datamosh: the horizontal component comes from
        // the first frame's grid and stays frozen, while the
        // vertical component follows the current frame.
        js << "  if (saved_grid === null) {\n"
              "    saved_grid = mvs.dup();\n"
              "  } else {\n"
              "    walk_mvs(mvs, function (mv, y, x, i) {\n"
              "      var saved = grid_at(saved_grid, y, x, i);\n"
              "      return saved ? [saved[0], mv[1]] : null;\n"
              "    });\n"
              "  }\n";
        break;
    }
    case PresetId::Vibrate: {
        // Add a sine offset whose phase advances with the frame
        // number: amplitude 1.0 at strength 0, 8.0 at strength 100.
        js << "  var vamp = " << fmt3(vibrate_amplitude(s)) << ";\n";
        js << "  var vph = frame_index * 0.5;\n";
        js << "  var vsh = Math.round(vamp * Math.sin(vph));\n";
        js << "  var vsv = Math.round(vamp * Math.sin(vph + 1.571));\n";
        js << "  walk_mvs(mvs, function (mv) {\n"
              "    return [mv[0] + vsh, mv[1] + vsv];\n"
              "  });\n";
        break;
    }
    case PresetId::Mirror: {
        // Flip the horizontal sign of a fraction of the vectors;
        // the fraction is strength/100, picked per cell from the
        // seeded rng so the result is reproducible.
        js << "  var mistr = " << s << ";\n";
        js << "  walk_mvs(mvs, function (mv) {\n"
              "    if (rng() * 100 < mistr) return [-mv[0], mv[1]];\n"
              "    return null;\n"
              "  });\n";
        break;
    }
    case PresetId::Shuffle: {
        // Swap each vector with a neighbouring cell's vector (the
        // neighbour is picked from the seeded rng). Strength is the
        // probability that a cell is swapped at all.
        js << "  var shsnap = mvs.dup();\n";
        js << "  var shstr = " << s << ";\n";
        js << "  walk_mvs(mvs, function (mv, y, x, i) {\n"
              "    if (rng() * 100 >= shstr) return null;\n"
              "    var shdx = Math.floor(rng() * 3) - 1;\n"
              "    var shdy = Math.floor(rng() * 3) - 1;\n"
              "    var shn = grid_at(shsnap, y + shdy, x + shdx, i);\n"
              "    return shn ? [shn[0], shn[1]] : null;\n"
              "  });\n";
        break;
    }
    case PresetId::Buffer: {
        // Apply the vectors from 8 frames ago. The first 8 frames
        // pass through unchanged while the queue fills.
        js << "  mv_queue.push(mvs.dup());\n"
              "  if (mv_queue.length > " << buffer_delay_frames() << ") {\n"
              "    var bufold = mv_queue.shift();\n"
              "    walk_mvs(mvs, function (mv, y, x, i) {\n"
              "      var b = grid_at(bufold, y, x, i);\n"
              "      return b ? [b[0], b[1]] : null;\n"
              "    });\n"
              "  }\n";
        break;
    }
    case PresetId::Delay: {
        // Like Buffer, but the delay grows with strength: 1 frame
        // at strength 0, 30 frames at strength 100.
        js << "  mv_queue.push(mvs.dup());\n"
              "  if (mv_queue.length > " << delay_frames(s) << ") {\n"
              "    var dlold = mv_queue.shift();\n"
              "    walk_mvs(mvs, function (mv, y, x, i) {\n"
              "      var d = grid_at(dlold, y, x, i);\n"
              "      return d ? [d[0], d[1]] : null;\n"
              "    });\n"
              "  }\n";
        break;
    }
    case PresetId::Stop: {
        // Scale every vector towards zero: unchanged at strength 0,
        // fully still (all zeros) at strength 100.
        js << "  var stf = " << fmt3(stop_factor(s)) << ";\n";
        js << "  walk_mvs(mvs, function (mv) {\n"
              "    return [Math.round(mv[0] * stf), Math.round(mv[1] * stf)];\n"
              "  });\n";
        break;
    }
    case PresetId::Sink: {
        // Pull every vector in the angle direction (default downwards when a==0).
        const int sk = sink_offset(s);
        const int sk_h = (a == 0) ? 0 : static_cast<int>(std::round(sk * cos_a));
        const int sk_v = (a == 0) ? sk : static_cast<int>(std::round(sk * sin_a));
        js << "  var sk_h = " << sk_h << ";\n"
              "  var sk_v = " << sk_v << ";\n"
              "  walk_mvs(mvs, function (mv) {\n"
              "    return [mv[0] + sk_h, mv[1] + sk_v];\n"
              "  });\n";
        break;
    }
    case PresetId::Fluid: {
        // Average motion vectors over a sliding window of past frames
        // (2 frames at strength 0, 16 frames at strength 100).
        // Smooths motion into a flowing fluid / melting effect.
        const int win = fluid_window_frames(s);
        js << "  mv_queue.push(mvs.dup());\n"
              "  if (mv_queue.length > " << win << ") {\n"
              "    mv_queue.shift();\n"
              "  }\n"
              "  if (mv_queue.length > 1) {\n"
              "    walk_mvs(mvs, function (mv, y, x, i) {\n"
              "      var sum_h = 0, sum_v = 0, count = 0;\n"
              "      for (var q = 0; q < mv_queue.length; q++) {\n"
              "        var cell = grid_at(mv_queue[q], y, x, i);\n"
              "        if (cell) {\n"
              "          sum_h += cell[0];\n"
              "          sum_v += cell[1];\n"
              "          count++;\n"
              "        }\n"
              "      }\n"
              "      return (count > 0) ? [Math.round(sum_h / count), Math.round(sum_v / count)] : null;\n"
              "    });\n"
              "  }\n";
        break;
    }
    case PresetId::Ripple: {
        // Spatial sine wave distortion along the angle direction,
        // simulating water ripple / wave distortion.
        const double amp = ripple_amplitude(s);
        const double freq = ripple_frequency(s);
        js << "  var ramp = " << fmt3(amp) << ";\n"
              "  var rfreq = " << fmt3(freq) << ";\n"
              "  var rphase = frame_index * 0.35;\n"
              "  var rcos = " << fmt3(cos_a) << ";\n"
              "  var rsin = " << fmt3(sin_a) << ";\n"
              "  walk_mvs(mvs, function (mv, y, x) {\n"
              "    var d = x * rcos + y * rsin;\n"
              "    var wave = Math.round(ramp * Math.sin(d * rfreq + rphase));\n"
              "    return [mv[0] + Math.round(wave * rcos), mv[1] + Math.round(wave * rsin)];\n"
              "  });\n";
        break;
    }
    case PresetId::Gravity: {
        // Detect blocks with motion and apply gravity acceleration along the angle
        // (default downwards when a==0) that accumulates frame by frame, capped at 32 px/frame.
        const double g_acc = gravity_acceleration(s);
        const int g_thresh = std::max(1, 16 - (s * 12) / 100);
        const double gh = (a == 0) ? 0.0 : cos_a;
        const double gv = (a == 0) ? 1.0 : sin_a;
        js << "  gravity_vel = Math.min(gravity_vel + " << fmt3(g_acc) << ", 32.0);\n"
              "  var g_thresh = " << g_thresh << ";\n"
              "  var gh = " << fmt3(gh) << ";\n"
              "  var gv = " << fmt3(gv) << ";\n"
              "  walk_mvs(mvs, function (mv) {\n"
              "    if ((mv[0] * mv[0] + mv[1] * mv[1]) >= g_thresh) {\n"
              "      return [mv[0] + Math.round(gravity_vel * gh), mv[1] + Math.round(gravity_vel * gv)];\n"
              "    }\n"
              "    return null;\n"
              "  });\n";
        break;
    }
    case PresetId::Transfer: {
        // Transfer motion vectors:
        // Captures motion vectors before the cut (or the first half in object mode)
        // and replays/transfers them across the frames after the cut.
        // Strength scales vector magnitude (0% = 0, 100% = original magnitude).
        // Angle rotates the transferred vectors if non-zero.
        const double f = static_cast<double>(s) / 100.0;
        if (cut_offset > 0) {
            js << "  var cut = " << cut_offset << ";\n"
                  "  var f = " << fmt3(f) << ";\n"
                  "  var rcos = " << fmt3(cos_a) << ";\n"
                  "  var rsin = " << fmt3(sin_a) << ";\n"
                  "  var use_angle = " << (a != 0 ? "true" : "false") << ";\n"
                  "  if (frame_index < cut) {\n"
                  "    if (mv_queue.length >= 120) mv_queue.shift();\n"
                  "    mv_queue.push(mvs.dup());\n"
                  "  } else {\n"
                  "    if (mv_queue.length > 0) {\n"
                  "      var src_k = (frame_index - cut) % mv_queue.length;\n"
                  "      var src_grid = mv_queue[src_k];\n"
                  "      walk_mvs(mvs, function (mv, y, x, i) {\n"
                  "        var saved = grid_at(src_grid, y, x, i);\n"
                  "        if (!saved) return null;\n"
                  "        var sh = saved[0] * f;\n"
                  "        var sv = saved[1] * f;\n"
                  "        if (use_angle) {\n"
                  "          var rh = sh * rcos - sv * rsin;\n"
                  "          var rv = sh * rsin + sv * rcos;\n"
                  "          return [Math.round(rh), Math.round(rv)];\n"
                  "        }\n"
                  "        return [Math.round(sh), Math.round(sv)];\n"
                  "      });\n"
                  "    }\n"
                  "  }\n";
        } else {
            const int hold = (total_frames > 0)
                ? std::max<int>(1, static_cast<int>(std::round(total_frames * (1.0 - static_cast<double>(s) / 100.0))))
                : 15;
            js << "  var hold = " << hold << ";\n"
                  "  var f = " << fmt3(f) << ";\n"
                  "  var rcos = " << fmt3(cos_a) << ";\n"
                  "  var rsin = " << fmt3(sin_a) << ";\n"
                  "  var use_angle = " << (a != 0 ? "true" : "false") << ";\n"
                  "  if (frame_index <= hold) {\n"
                  "    if (mv_queue.length >= 120) mv_queue.shift();\n"
                  "    mv_queue.push(mvs.dup());\n"
                  "  } else {\n"
                  "    if (mv_queue.length > 0) {\n"
                  "      var src_k = (frame_index - hold - 1) % mv_queue.length;\n"
                  "      var src_grid = mv_queue[src_k];\n"
                  "      walk_mvs(mvs, function (mv, y, x, i) {\n"
                  "        var saved = grid_at(src_grid, y, x, i);\n"
                  "        if (!saved) return null;\n"
                  "        var sh = saved[0] * f;\n"
                  "        var sv = saved[1] * f;\n"
                  "        if (use_angle) {\n"
                  "          var rh = sh * rcos - sv * rsin;\n"
                  "          var rv = sh * rsin + sv * rcos;\n"
                  "          return [Math.round(rh), Math.round(rv)];\n"
                  "        }\n"
                  "        return [Math.round(sh), Math.round(sv)];\n"
                  "      });\n"
                  "    }\n"
                  "  }\n";
        }
        break;
    }
    }

    js << glitch_frame_close();
    return js.str();
}

std::string generate_preset_js(const Settings& settings) {
    return generate_preset_js(settings.preset, settings.strength, settings.seed,
                              settings.angle, settings.cut_offset,
                              settings.interval.frame_count());
}

} // namespace hybrid
