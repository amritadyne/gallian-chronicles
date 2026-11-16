// The game's own sound volumes in settings.ini: see vc3_audio.cpp.
#pragma once

namespace portablekit {
class HleRegistrar;
}

namespace portablekit::vc3 {

// Keeps the game's Music, SFX and Voice volumes (OPTIONS) and settings.ini's
// vc3.music_volume, vc3.sfx_volume and vc3.voice_volume in step. OPTIONS
// closing (cmake/vc3_sites.cmake) hands the player's choice to settings.ini.
void install_audio(HleRegistrar &hle);

}  // namespace portablekit::vc3
