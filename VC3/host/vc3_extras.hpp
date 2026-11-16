// EXTRAS: "Unlock Extras" in place of Password: see vc3_extras.cpp.
#pragma once

#include "hle/hle_common.hpp"

namespace portablekit::vc3 {

// VC3_NO_UNLOCK_EXTRAS=1 leaves EXTRAS as the game has it.
void install_extras(Runtime &rt);

}  // namespace portablekit::vc3
