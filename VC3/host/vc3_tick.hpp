// VC2 at a higher frame rate, first experiment: see vc3_tick.cpp.
#pragma once

#include "hle/hle_common.hpp"

#include <string>

namespace portablekit {
namespace vc3 {

// VC3_TICK_HZ=<rate> (30..1000) runs the game's frame clock at that rate.
// Unset: the PSP's 59.94 Hz, as before.
void install_tick(Runtime &rt);
// Paces the game's per-frame vblank wait by our clock (with VC3_TICK_HZ).
void register_tick(HleRegistrar &hle);
// Changes the frame rate while the game runs: "0" (the game's own),
// "uncapped" or 30..1000. False where it cannot (Japan) or for a bad value.
bool set_frame_rate(const std::string &value);
// Whether the frame rate can change (and be above the game's) on this release.
[[nodiscard]] bool frame_rate_live();
// The game's step this run, in PSP frames (1/59.94 s): 59.94 / rate when the
// step is corrected, else 0 (the game's own: 1, or 2 in combat).
[[nodiscard]] float corrected_step();
// For the frame census: the step alternates frame by frame (x1, x1.5) while
// on, so a steady per-frame change is one that ignores the step.
void set_step_wobble(bool on);
[[nodiscard]] bool step_wobble();

}  // namespace vc3
}  // namespace portablekit
