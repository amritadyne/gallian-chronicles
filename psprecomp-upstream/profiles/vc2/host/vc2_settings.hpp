#pragma once

#include <string>
#include <string_view>

namespace portablekit::vc2::options {

enum class Key {
    TickHz, Lod, Controls, StickDeadZone, Prompts, Discord, Backgrounds,
    MusicVolume, SfxVolume, VoiceVolume,  // "0".."10", or empty: the game's own (vc2_audio.cpp)
    // Online play (vc2_online.cpp): "on" or "off" ("host"/"join", from before
    // friend codes, count as on); the old invite code (unused); the UDP port;
    // router port mapping and STUN "on"/"off"; this player's secret key (64
    // hex) and own friend code, and up to three friends' codes (all set by the
    // launcher, Network page).
    Online, OnlineCode, OnlinePort, OnlineRouter, OnlineStun,
    OnlineKey, OnlineMyCode, OnlineFriend1, OnlineFriend2, OnlineFriend3,
    // Voices from another release's disc (vc2_voice.cpp): that disc image's
    // path, and "on"/"off" (launcher, Audio page).
    VoiceDisc, VoiceReplace,
    Count
};

// Loaded once from the same data directory as PortableKit. Environment values
// win, including diagnostic values outside the launcher's choice list.
void load();
[[nodiscard]] const std::string &get(Key key);
[[nodiscard]] bool overridden(Key key);

// Main/game thread only. This changes the stored choice; the caller must apply
// its runtime effect. Environment-locked choices and invalid values are refused.
[[nodiscard]] bool set(Key key, std::string_view value);
[[nodiscard]] bool dirty();
// Called explicitly on leaving/confirming the game's Options screen, never at
// startup or shutdown. Re-reads the file to preserve all other owners' keys.
// On failure the dirty values remain available for retry.
[[nodiscard]] bool save();

} // namespace portablekit::vc2::options
