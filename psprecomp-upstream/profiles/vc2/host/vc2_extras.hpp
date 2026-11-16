// EXTRAS: "Enable Bonus Content" in place of Link Data and Password: see vc2_extras.cpp.
#pragma once

#include "hle/hle_common.hpp"

namespace portablekit::vc2 {

// Europe. VC2_NO_BONUS_CONTENT=1 leaves EXTRAS as it was.
void install_extras(Runtime &rt);

}  // namespace portablekit::vc2
