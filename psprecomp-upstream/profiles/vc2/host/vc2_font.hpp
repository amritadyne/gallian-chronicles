// VC2's text font (sceLibFont), host code of the port's own: see vc2_font.cpp.
#pragma once

namespace portablekit {
class HleRegistrar;
namespace vc2 {

// Binds the game's sceLibFont calls to this port's font layer, replacing
// PortableKit's (VC2_PORTABLEKIT_FONT=1 keeps PortableKit's).
void register_font(HleRegistrar &hle);

}  // namespace vc2
}  // namespace portablekit
