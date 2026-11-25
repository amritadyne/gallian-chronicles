#pragma once

// The soundtrack import as a whole: `<game> --import-soundtrack <folder>`.
// Finds the album tracks the game uses among the player's files, builds each
// game track from them (soundtrack_import.hpp) and writes one FLAC per track,
// named by the track's key, into the soundtrack folder the game plays from.

#include "audio/soundtrack_import.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace portablekit::audio::soundtrack {

// One of the game's music tracks: a whole ATRAC3 / ATRAC3plus WAVE file.
struct GameTrack {
    std::string name;
    std::vector<std::uint8_t> wave;
};

// The key the game's music player names this track by (hle_atrac.cpp
// track_key): FNV-1a 64 of the first 4 KiB of its data. Empty when too short.
[[nodiscard]] std::string wave_key(const std::vector<std::uint8_t> &wave);

// The track decoded and moved into the game's own sample positions. Empty when
// it can't be decoded.
[[nodiscard]] Stereo decode_game_track(const std::vector<std::uint8_t> &wave);

// Where the import writes and the game plays from: soundtrack/ in the game
// folder (<prefix>_GAME_DIR or <prefix>_DATA_DIR) when one is set, else in the
// per-user data folder.
[[nodiscard]] std::filesystem::path default_soundtrack_folder();

// Runs the import and prints its progress ("Importing the soundtrack: N%") and
// a line per track. Writes `<out>/<key>.flac` per track it could build and
// `<out>/soundtrack.txt`, the report. Returns 0 when at least one track was built.
int run_import(const std::filesystem::path &album_folder, const std::filesystem::path &out,
               const std::vector<GameTrack> &tracks, const std::vector<Entry> &table);

} // namespace portablekit::audio::soundtrack
