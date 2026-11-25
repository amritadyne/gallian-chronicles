#pragma once

#include "audio/soundtrack_import.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include "audio/soundtrack_import_run.hpp"

namespace vc3 {

// Every VC3 music track the soundtrack import can replace.
[[nodiscard]] const std::vector<portablekit::audio::soundtrack::Entry> &soundtrack_table();

// GameProfile::soundtrack_music: none. VC3's music is in INSDIR/DATA.BIN, a PGD
// file decrypted with a key only the running game hands over, so the import
// builds every track from the album alone (Entry::game_frames / game_rms).
bool soundtrack_music(const std::filesystem::path &image, std::vector<portablekit::audio::soundtrack::GameTrack> &tracks,
                      std::string &error);

} // namespace vc3
