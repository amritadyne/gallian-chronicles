#pragma once

namespace portablekit::vc2 {
// The level of detail changed (the in-game Settings screen): the render
// parameter blocks loaded so far get the new scale over their own distances.
void lod_apply_live();
}  // namespace portablekit::vc2
