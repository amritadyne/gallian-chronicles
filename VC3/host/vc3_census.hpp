// Frame census for the high-frame-rate work: see vc3_census.cpp.
#pragma once

#include "hle/hle_common.hpp"

namespace portablekit {
namespace vc3 {

// VC3_CENSUS=<folder>: F10 in game records what changes every game frame and
// (in the trace build) which functions change it.
void register_census(HleRegistrar &hle);
// A swap-hunt mark made by the program (the texture watch of vc3_native_gu,
// VC3_WATCH_TEXTURE) instead of F12; nothing outside a swap hunt.
void census_mark();

}  // namespace vc3
}  // namespace portablekit
