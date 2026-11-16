#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The in-game Settings screen (vc3_settings_menu.cpp): the title menu's
// SETTINGS entry (the PSP's old INSTALL slot) opens the game's own Options
// screen in settings mode, whose rows are then ours: drawn from the Options
// screen's own pieces (vc3_native_gu.cpp, "The Settings screen"), driven by the
// pad here, applied at once and saved to settings.ini when the player leaves.
// Europe only (the title menu's hooks are patched in its corpus).
namespace portablekit::vc3::settings_menu {

// The title menu's SETTINGS was chosen: the next Options screen is ours.
void request();
// The Options screen is on screen this frame (the draw code). While a request
// is pending (or the screen is ours) this makes it ours; when it has gone for a
// moment the screen is given back and any change saved.
void options_frame(bool shown);
// The Options screen is ours: its rows are drawn from view(), the pad is read
// here and the game sees no buttons (Back aside, when leaving).
[[nodiscard]] bool active();
// The draw code could not build the screen: the game's own Options screen
// stays, with its own controls (any change made is saved).
void give_back();

// One pad read (vc3_controls.cpp remap, every preset): the buttons the game
// should see. The menu acts on what was pressed; while it is active the game
// gets nothing but the Back press that closes its Options screen.
// `stick_x`/`stick_y`: the left stick, 0..255 (128 centred).
[[nodiscard]] std::uint32_t filter_buttons(std::uint32_t buttons, int stick_x, int stick_y);
// The last filter_buttons wants the left stick centred for the game (a
// direction it took for itself is still held).
[[nodiscard]] bool stick_taken();

// "Settings" under "Options" in the game's menus (the academy menu, the
// battle's command menu, ...): the draw code finds such a menu each frame and
// draws a Settings row under its Options row; the cursor on that row is ours
// (the game's own stays on Options, so choosing it opens the game's Options
// screen, which then is the Settings screen). `focus`: the game's cursor row,
// -1 unknown; `options`: the Options row; `rows`: the menu's rows (0: none
// shown this frame).
void entry_frame(int rows, int options, int focus);
// The cursor is on our Settings row (the draw code moves the highlight there).
[[nodiscard]] bool entry_on();
// The game played a sound effect (its sound function, VC3 0x08816500; a0 the
// sound's id; cmake/vc3_sites.cmake). One played just after the game moved its
// own cursor in such a menu is its cursor sound.
void sound_played(std::uint32_t id);
// A move of ours (Options to Settings and back, kept from the game) wants the
// game's cursor sound: its id (learned as above; 1 until then). Once a move.
[[nodiscard]] bool take_cursor_sound(std::uint32_t &id);

struct RowView {
    std::string label;
    std::vector<std::string> values;  // boxes (up to three), or the one value of a stepper
    int selected = 0;                  // the box chosen (boxes)
    bool stepper = false;              // one box with arrows (more than three values)
    bool first = false, last = false;  // stepper at either end
    bool slider = false;               // the Options screen's slider
    float fraction = 0.0f;             // slider: 0 at min, 1 at max
};
struct View {
    std::string title;
    std::vector<std::string> tabs;
    int tab = 0;
    std::vector<RowView> rows;
    int focus = 0;
    std::string help;  // the description bar
};
[[nodiscard]] View view();

}  // namespace portablekit::vc3::settings_menu

// Applies a video.* setting now (vc3_native_gu.cpp, which holds the renderer).
// Returns false when the value is not understood.
namespace portablekit::vc3 {
bool apply_video_setting(const std::string &key, const std::string &value);
}
