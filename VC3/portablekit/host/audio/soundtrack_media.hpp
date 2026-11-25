#pragma once

// The FFmpeg side of the soundtrack import: reading the player's audio files
// (any format FFmpeg reads: FLAC, MP3, AAC/ALAC in M4A, Ogg, Opus, WAV...),
// the pitch-kept tempo change, and the FLAC files the import writes and the game
// plays. Needs libavformat, libavcodec, libavfilter and libswresample.

#include "audio/soundtrack_import.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace portablekit::audio::soundtrack {

// What the import can tell about a file without decoding it.
struct FileInfo {
    double seconds{};
    std::string title, album;
    int disc{}, track{}; // 0 when the file doesn't say
};

[[nodiscard]] bool media_available() noexcept;

[[nodiscard]] std::optional<FileInfo> probe(const std::filesystem::path &file);

// The whole file as 44.1 kHz stereo (resampled and up/down-mixed as needed).
[[nodiscard]] std::optional<Stereo> decode(const std::filesystem::path &file, std::string &error);

// `input` played at `tempo` times its speed, pitch kept (FFmpeg's atempo).
[[nodiscard]] Stereo change_tempo(const Stereo &input, double tempo);

// Writes interleaved 16-bit stereo 44.1 kHz as FLAC. Replaces `file` only once
// the new one is complete.
bool write_flac(const std::filesystem::path &file, const std::vector<std::int16_t> &pcm, std::string &error);

// Reads a FLAC (or any file) back as interleaved 16-bit stereo at 44.1 kHz.
[[nodiscard]] std::optional<std::vector<std::int16_t>> read_pcm16(const std::filesystem::path &file, std::string &error);

} // namespace portablekit::audio::soundtrack
