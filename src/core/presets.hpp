// presets.hpp — ffedit JavaScript generation for the M1 presets.
//
// The generated script follows the official ffedit contract
// (https://ffglitch.org/docs/0.10.2/ffedit/ and
// https://ffglitch.org/docs/0.10.2/quickjs/):
//   - it exports glitch_frame(frame, stream), called once per frame;
//   - frame.mv.forward (or frame.mv_delta.forward) is an MV2DArray
//     whose cells are null or MV objects (readable as mv[0]/mv[1]);
//     a 4MV macroblock cell holds four sub-block MVs. Cells are
//     mutated in place with mv.assign(MV(h, v)); assigning a plain
//     array to a cell fails in ffedit, so the generator never does it.
//     Verified against real ffedit 0.10.2 on 2026-11-18.
//   - overflow is set to "truncate" so out-of-range values are
//     truncated instead of erroring.
//
// QuickJS has no seeded Math.random, so the script carries its own
// mulberry32(seed) generator. Position is always tracked as (y, x, i),
// never as visit order, so the result does not depend on how many null
// cells came before.
//
// Strength mappings below are this project's own decisions (design doc
// section 8). They have not been tuned against real footage yet; that
// is M4 work.

#pragma once

#include <cstdint>
#include <string>

#include "settings.hpp"

namespace hybrid {

// Generate the ffedit script for one preset. Deterministic: the same
// (preset, strength, seed, angle, cut_offset) always produces the same text.
std::string generate_preset_js(PresetId preset, int strength, std::uint32_t seed,
                               int angle = 0, std::int64_t cut_offset = -1,
                               std::int64_t total_frames = 0);
std::string generate_preset_js(const Settings& settings);

// The fixed vector MV固定 writes, derived from the seed. Exposed so
// tests can check the JS embeds the same numbers.
void fixed_vector(std::uint32_t seed, int& out_h, int& out_v) noexcept;

// Amplify factor for a strength value: 1.0 at 0, 5.0 at 100.
double amplify_factor(int strength) noexcept;
// Random range for a strength value: 1 at 0, 11 at 100.
int random_range(int strength) noexcept;

// M4 strength mappings (design doc section 8.2). These are this
// project's own decisions; the visual result on real footage has
// not been tuned yet. The JS generator uses the same helpers, so
// the unit tests below cover the exact numbers ffedit receives.
//   Zoom: scale for a cell whose centre distance is dist_ratio
//     (0 at the centre, 1 at the farthest corner) of the frame:
//     1.0 at strength 0, 2.0 at the corners at strength 100.
//   Shear: horizontal offset added to a cell in row y of a grid
//     with `rows` rows: 0 at strength 0, about +/-rows/4 at the
//     top/bottom rows at strength 100.
//   Shift: horizontal offset for the whole frame, from the seed:
//     0 at strength 0, +/-8 at strength 100.
//   Delay: how many frames the MVs are delayed: 1 at strength 0,
//     30 at strength 100. Buffer is a fixed 8-frame delay.
//   Vibrate: sine amplitude added to MVs: 1.0 at strength 0,
//     8.0 at strength 100.
//   Sink: downward offset added to vertical MVs: 0 at strength 0,
//     8 at strength 100.
//   Stop: factor MVs are scaled towards zero: 1.0 at strength 0,
//     0.0 at strength 100.
//   Fluid: number of past frames to average: 2 at strength 0,
//     16 at strength 100.
//   Ripple: amplitude and spatial frequency of sine wave across rows.
//   Gravity: downward acceleration added per frame to active vectors:
//     0.2 at strength 0, 2.0 at strength 100.
double zoom_factor(int strength, double dist_ratio) noexcept;
int shear_offset(int strength, int y, int rows) noexcept;
int shift_offset(std::uint32_t seed, int strength) noexcept;
int delay_frames(int strength) noexcept;
int buffer_delay_frames() noexcept;
double vibrate_amplitude(int strength) noexcept;
int sink_offset(int strength) noexcept;
double stop_factor(int strength) noexcept;
int fluid_window_frames(int strength) noexcept;
double ripple_amplitude(int strength) noexcept;
double ripple_frequency(int strength) noexcept;
double gravity_acceleration(int strength) noexcept;

} // namespace hybrid
