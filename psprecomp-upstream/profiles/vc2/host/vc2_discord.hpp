// Discord Rich Presence through the Discord app's local pipe: see vc2_discord.cpp.
#pragma once

namespace portablekit::vc2 {

// Starts the presence thread (after the settings are loaded). Does nothing
// when Discord status is off or no application ID is set.
void start_discord();
// Turns Discord status on or off while the game runs (in-game Settings).
void set_discord(bool on);

}  // namespace portablekit::vc2
