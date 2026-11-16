// Discord Rich Presence through the Discord app's local pipe: see vc3_discord.cpp.
#pragma once

#include <cstdint>

namespace portablekit::vc3 {

// Starts the presence thread (after the settings are loaded). Does nothing
// when Discord status is off or no application ID is set.
void start_discord();
// Turns Discord status on or off while the game runs (in-game Settings).
void set_discord(bool on);
// A message the game fetched by its id (vc3_text.cpp): where the player is
// (Base Camp's, the march map's or a battle's messages).
void discord_note_message(std::uint32_t id);

}  // namespace portablekit::vc3
