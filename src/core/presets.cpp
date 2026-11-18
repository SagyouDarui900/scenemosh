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
    js << "var mv_queue = [];\n"; // Buffer / Delay: past grids, oldest first
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

std::string generate_preset_js(PresetId preset, int strength, std::uint32_t seed) {
    const int s = std::clamp(strength, 0, 100);
    std::ostringstream js;
    js << common_preamble(preset, s, seed);
    js << glitch_frame_open();

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
        fixed_vector(seed, fh, fv);
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
        // Classic datamosh: keep the first frame's vectors and reuse them
        // for every later frame, so motion smears forward in time.
        js << "  if (saved_grid === null) {\n"
              "    saved_grid = mvs.dup();\n"
              "  } else {\n"
              "    walk_mvs(mvs, function (mv, y, x, i) {\n"
              "      var saved = grid_at(saved_grid, y, x, i);\n"
              "      return saved ? [saved[0], saved[1]] : null;\n"
              "    });\n"
              "  }\n";
        break;
    }
    case PresetId::DriftDelta: {
        // mv_delta only: add a small per-frame drift that accumulates,
        // so motion slowly slides in one direction. The direction comes
        // from the seed; the step size grows with strength.
        int dh = 0, dv = 0;
        fixed_vector(seed, dh, dv);
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
        // One constant horizontal offset for the whole frame, from
        // the seed: 0 at strength 0, +/-8 at strength 100.
        js << "  var shdh = " << shift_offset(seed, s) << ";\n";
        js << "  walk_mvs(mvs, function (mv) {\n"
              "    return [mv[0] + shdh, mv[1]];\n"
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
        // Pull every vector downwards: 0 at strength 0, +8 vertical
        // at strength 100.
        js << "  var skoff = " << sink_offset(s) << ";\n";
        js << "  walk_mvs(mvs, function (mv) {\n"
              "    return [mv[0], mv[1] + skoff];\n"
              "  });\n";
        break;
    }
    }

    js << glitch_frame_close();
    return js.str();
}

} // namespace hybrid
