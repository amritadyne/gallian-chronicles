// VC2's text font (sceLibFont), host code of the port's own: see vc3_font.cpp.
#pragma once

namespace portablekit {
class HleRegistrar;
namespace vc3 {

// Binds the game's sceLibFont calls to this port's font layer, replacing
// PortableKit's (VC3_PORTABLEKIT_FONT=1 keeps PortableKit's).
void register_font(HleRegistrar &hle);

}  // namespace vc3
}  // namespace portablekit
