#pragma once

// Reads VC2's music tracks out of the player's disc image, for the soundtrack
// import: ODIN.CPK's M_STR_NN_MM.ATX files (decompressed and decrypted as the
// game does), each track's pieces joined back into one ATRAC3plus WAVE file.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "audio/soundtrack_import_run.hpp"

namespace vc2 {

struct MusicTrack {
    std::string name;                // "STR_01"
    std::vector<std::uint8_t> wave;  // the whole RIFF WAVE file (ATRAC3plus)
};

// Every music track on the disc, in name order. Empty with `error` set when the
// image can't be read.
[[nodiscard]] std::vector<MusicTrack> read_music(const std::filesystem::path &image, std::string &error);

// GameProfile::soundtrack_music: read_music for the soundtrack import.
bool soundtrack_music(const std::filesystem::path &image, std::vector<portablekit::audio::soundtrack::GameTrack> &tracks,
                      std::string &error);

// Joins one track's pieces (each: a 32-byte APA3 header whose second word is the
// size of the data that follows, then the data), in piece order, and cuts the
// result at the RIFF size. Empty when they don't make a WAVE file.
[[nodiscard]] std::vector<std::uint8_t> join_pieces(const std::vector<std::vector<std::uint8_t>> &pieces);

} // namespace vc2
