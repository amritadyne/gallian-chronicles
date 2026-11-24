// Voices from another release's disc: see vc2_voice.cpp.
#pragma once

namespace portablekit::vc2 {

// With settings.ini's vc2.voice_replace "on" and vc2.voice_disc naming another
// Valkyria Chronicles II disc image (the Japanese one for a Japanese dub on the
// European or North American release, or the other way round), the game's
// voice archive (STREAM/OD_VOICE.AFS) is read with that disc's recordings in
// place of this release's, line by line, and each movie (MOVIE/*.PMF) keeps
// this release's picture with that disc's sound. Nothing happens otherwise. Logs
// [vc2-voice]. Called once at start-up, before the game opens any file.
void install_voice();

}  // namespace portablekit::vc2
