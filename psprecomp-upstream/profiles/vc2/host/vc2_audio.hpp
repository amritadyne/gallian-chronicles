// The game's own sound volumes in settings.ini: see vc2_audio.cpp.
#pragma once

namespace portablekit {
class HleRegistrar;
}

namespace portablekit::vc2 {

// Keeps the game's Music, SFX and Voice volumes (OPTIONS > Preferences) and
// settings.ini's vc2.music_volume, vc2.sfx_volume and vc2.voice_volume in
// step, once a frame. Europe and North America (Japan: nothing).
void install_audio(HleRegistrar &hle);

// The OPTIONS screen built its Preferences rows (vc2_extras.cpp): a change to
// the volumes from now on is the player's, and goes to settings.ini.
void audio_options_opened();

}  // namespace portablekit::vc2
