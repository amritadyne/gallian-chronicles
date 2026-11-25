#pragma once

// Soundtrack import: builds, for one of the game's music tracks, a version made
// from the player's own copy of the album track, in the game's own sample
// positions (sample p of the result plays where the game would play its own
// sample p), so the game's loop points and timing still apply.
//
// This part does the audio work only (alignment, level, edits). Reading the
// disc, decoding the player's files and writing the results are elsewhere
// (soundtrack_media.cpp, the game's import entry point). Nothing here holds or
// ships any audio: the per-game tables are track names, lengths and offsets.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace portablekit::audio::soundtrack {

inline constexpr int kRate = 44'100;
// A frame counts as sound from this level (about -54 dBFS) on.
inline constexpr int kOnsetLevel = 64;

// Interleaved stereo at 44.1 kHz, on the 16-bit scale (full scale = 32768).
struct Stereo {
    std::vector<float> samples;
    [[nodiscard]] std::size_t frames() const noexcept { return samples.size() / 2; }
    [[nodiscard]] float left(std::size_t i) const noexcept { return samples[2 * i]; }
    [[nodiscard]] float right(std::size_t i) const noexcept { return samples[2 * i + 1]; }
};

// How a game track is made from the album track.
enum class Method {
    Exact,  // the same recording at one offset: sample-exact at several points
    Mix,    // the same timing but a different mix: one offset, matched less tightly
    Recipe, // the game's track is an edit of the album recording (see Recipe)
};

// One stretch of a recipe, in the game's sample positions [from, to).
struct Section {
    enum class Source {
        Album,          // album sample = game sample + lag (lag in the reference rip)
        Game,           // the game's own audio
        AlbumStretched, // album from album_start (reference rip), tempo-changed with pitch kept;
                        // stretched sample q plays at game position from + q - lag, or, with
                        // search > 0, where it matches the game's audio best within +-search
    };
    Source source{Source::Album};
    std::int64_t from{};
    std::int64_t to{};
    std::int64_t lag{};
    double album_start{}; // seconds
    double tempo{1.0};    // playback speed of the album part (below 1 = slower)
    std::int64_t search{};
};

struct Recipe {
    std::vector<Section> sections;
    std::int64_t crossfade{};   // samples on each side of a join
    bool equal_power{};         // sin^2 / cos^2 crossfades instead of straight lines
    bool level_each_section{};  // match each section to the game's level over its own span
    // The game's audio is a different render of the album recording, so it can't
    // pin the rip's offset: take it from where the album's sound starts instead
    // (rips mostly differ by padding there), then check the match loosely.
    bool loose{};
    // After the level match: the game's own audio for join_half samples each side
    // of every join, faded in and out over join_fade samples (0 = not used).
    std::int64_t join_half{};
    std::int64_t join_fade{};
};

struct Entry {
    std::string track;   // the game's name for it (VC2: STR_NN)
    std::string key;     // the runtime key: FNV-1a 64 of the first 4 KiB of its data
    std::string number;  // disc-track on the album, "1-02"
    std::string title;
    std::string album;
    double album_seconds{}; // length of the album track in the reference rip
    std::int64_t album_onset{}; // first frame of the reference rip at or above kOnsetLevel
    Method method{Method::Exact};
    std::int64_t lag{};  // Exact / Mix: album sample = game sample + lag (reference rip)
    Recipe recipe;       // Method::Recipe
    // For games whose music the import can't read from the disc (VC3: its data
    // is decrypted with a key only the running game hands over): the game
    // track's length and loudness (RMS, 16-bit scale), so it can be built from
    // the album alone (build_from_reference). 0: the game's audio is needed.
    std::int64_t game_frames{};
    double game_rms{};
};

// Tempo change with pitch kept: returns `input` played at `tempo` times its speed.
using Stretch = std::function<Stereo(const Stereo &input, double tempo)>;

struct Result {
    bool ok{};
    std::string reason;            // why the game's own music is kept, when !ok
    std::int64_t lag{};            // the offset used (Exact / Mix; Recipe: its first album section)
    std::int64_t rip_offset{};     // lag - the table's lag: how this rip differs from the reference
    double score{};                // the weakest correlation that was accepted
    double gain_db{};
    std::vector<std::int16_t> pcm; // interleaved stereo, game positions, the game track's length
};

// Builds the game track `game` (decoded, in game positions) from `album`.
[[nodiscard]] Result build(const Entry &entry, const Stereo &game, const Stereo &album, const Stretch &stretch);

// First frame at or above kOnsetLevel (-1 when silent throughout).
[[nodiscard]] std::int64_t onset(const Stereo &album);

// Builds the game track from the album alone, for an entry with game_frames:
// the rip's offset from the reference is taken from where the album's sound
// starts, the level from game_rms. Recipes may only use album sections.
[[nodiscard]] Result build_from_reference(const Entry &entry, const Stereo &album);

// How well `album` lines up with `game` at the table's offset: the median
// correlation at a few points after a search of +-search samples around it.
// Used to pick between several candidate files. -1 when they can't be compared.
[[nodiscard]] double quick_score(const Entry &entry, const Stereo &game, const Stereo &album);

// --- parts, exposed for the tests ---------------------------------------------------

// The best offset of the window game[at, at+length) inside album, searched over
// album positions at + lag_guess +- search, on the mono mix. Returns the lag
// (album position - game position) and the normalised correlation.
struct LagFound {
    std::int64_t lag{};
    double score{-1.0};
};
[[nodiscard]] LagFound find_lag(const Stereo &game, const Stereo &album, std::int64_t at, std::int64_t length,
                                std::int64_t lag_guess, std::int64_t search, int decimate = 1);

} // namespace portablekit::audio::soundtrack
