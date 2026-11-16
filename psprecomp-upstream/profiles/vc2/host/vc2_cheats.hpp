#pragma once

#include "hle/hle_common.hpp"

namespace portablekit::vc2 {

// Texture-dump helpers (Europe only; off unless asked for): VC2_CHEAT_GODMODE=1
// keeps battle units' HP at 9999, VC2_CHEAT_AP=1 keeps their AP full.
// Logs [vc2-cheats]. See vc2_cheats.cpp.
void install_cheats(Runtime &rt);

}  // namespace portablekit::vc2
