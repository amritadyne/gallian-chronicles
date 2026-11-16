// The in-game Settings screen: model, pad and settings (see vc3_settings_menu.hpp).
//
// The rows are the launcher's (launcher/ValkyriaLauncher.cpp), limited to the
// settings that change while the game runs, with short labels in plain ASCII:
// the game's font sheet holds the 96 printable ASCII characters only, and its
// value boxes are 66 PSP pixels wide. Up to three values are boxes, as the
// game's own rows; more are one box stepped with left / right.
//
// Pad (PSP buttons after the controls layer): up / down a row, left / right a
// value, L / R a tab, Triangle the row's default, Circle back (saves, and the
// game's Options screen is closed with its own Back). Held up / down repeats.

#include "vc3_settings_menu.hpp"
#include "vc3_settings.hpp"
#include "vc3_lod.hpp"
#include "vc3_glyphs.hpp"
#include "vc3_controls.hpp"
#include "vc3_tick.hpp"
#include "vc3_discord.hpp"

#include "profile.hpp"
#include "install/user_data.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace portablekit::vc3::settings_menu {
namespace {

struct Choice {
    const char *label;
    const char *value;
};
struct Row {
    const char *label;
    const char *key;
    const char *fallback;
    std::vector<Choice> choices;
    const char *help;
    bool slider = false;  // drawn as the Options screen's volume slider (min to max)
};
struct Tab {
    const char *name;
    std::vector<Row> rows;
};

const std::vector<Tab> &tabs() {
    static const std::vector<Tab> t = {
        {"Display",
         {
             {"Display Mode", "video.display_mode", "window",
              {{"Window", "window"}, {"Borderless", "borderless"}, {"Full", "fullscreen"}},
              "Window, a borderless window over the whole screen, or fullscreen."},
             {"Aspect Ratio", "video.aspect", "original",
              {{"Original", "original"}, {"Fit Window", "fill"}, {"16:9", "16:9"}, {"16:10", "16:10"}, {"4:3", "4:3"},
               {"21:9", "21:9"}, {"32:9", "32:9"}, {"Stretch", "stretch"}},
              "Original: the PSP's picture. The others widen the 3D view to that shape; menus keep the PSP's."},
             {"Resolution", "video.resolution", "window",
              {{"Match Window", "window"}, {"272 (PSP)", "272"}, {"480", "480"}, {"720", "720"}, {"768", "768"},
               {"900", "900"}, {"1080", "1080"}, {"1200", "1200"}, {"1440", "1440"}, {"1600", "1600"},
               {"1800", "1800"}, {"2160 (4K)", "2160"}, {"2880 (5K)", "2880"}},
              "Lines the game is drawn at; the width follows the aspect ratio. Higher costs more GPU time."},
             {"Backgrounds", "vc3.backgrounds", "original",
              {{"Original", "original"}, {"Zoom", "zoom"}, {"Zoom All", "zoom-movies"}},
              "Wide shapes: full-screen pictures zoomed to fill the window. Zoom All: movies too."},
             {"Frame Rate", "vc3.tick_hz", "0",
              {{"Game Rate", "0"}, {"60", "60"}, {"90", "90"}, {"120", "120"}, {"144", "144"}, {"165", "165"},
               {"240", "240"}, {"Uncapped", "uncapped"}},
              "Smoother motion at normal game speed. Game Rate: the PSP's 30 / 60."},
             {"VSync", "video.present_mode", "vsync",
              {{"On", "vsync"}, {"Triple", "mailbox"}, {"Off", "immediate"}},
              "On waits for the display. Triple: off, without tearing. Off may tear."},
         }},
        {"Graphics",
         {
             {"Texture Filter", "video.sharp_textures", "0", {{"Smooth", "0"}, {"Pixelated", "1"}},
              "How textures are sampled: smooth (bilinear) or pixelated (nearest)."},
             {"Upscaling", "video.sharp_screen", "0", {{"Smooth", "0"}, {"Sharp", "1"}},
              "How the picture is scaled to the window."},
             {"Level of Detail", "vc3.lod", "original", {{"Original", "original"}, {"Full", "full"}},
              "Original swaps models for simpler ones at the PSP's distances. Full keeps every model detailed."},
             {"FPS Counter", "video.performance", "off", {{"Off", "off"}, {"Simple", "fps"}, {"Detailed", "overlay"}},
              "Simple shows the frame rate alone. Detailed adds frame times, speed and a graph."},
             {"Texture Pack", "video.texture_pack", "1", {{"On", "1"}, {"Off", "0"}},
              "Draw the texture pack's images in place of the game's."},
         }},
        {"Controls",
         {
             {"Camera Speed", "input.camera_speed", "190.00",
              {{"90", "90.00"}, {"110", "110.00"}, {"130", "130.00"}, {"140", "140.00"}, {"150", "150.00"},
               {"170", "170.00"}, {"190 (default)", "190.00"}, {"220", "220.00"}, {"260", "260.00"}, {"310", "310.00"},
               {"360", "360.00"}},
              "How fast the right stick turns the camera, in degrees a second.", true},
             {"Mouse Sensitivity", "input.mouse_sensitivity", "0.10",
              {{"0.05", "0.05"}, {"0.07", "0.07"}, {"0.10 (default)", "0.10"}, {"0.12", "0.12"}, {"0.15", "0.15"},
               {"0.17", "0.17"}, {"0.20", "0.20"}, {"0.25", "0.25"}, {"0.30", "0.30"}},
              "How far the camera turns for each movement of the mouse.", true},
             {"Camera Up-Down", "input.invert_camera_y", "0", {{"Normal", "0"}, {"Reverse", "1"}},
              "Normal: push up to look up."},
             {"Camera Right-Left", "input.invert_camera_x", "0", {{"Normal", "0"}, {"Reverse", "1"}},
              "Normal: push right to turn right."},
             {"Left Stick Deadzone", "vc3.stick_dead_zone", "original",
              {{"5%", "5"}, {"10%", "10"}, {"15%", "15"}, {"20%", "20"}, {"25%", "25"}, {"30%", "30"},
               {"35%", "35"}, {"40%", "40"}, {"47% (original)", "original"}},
              "How far the left stick is pushed before your unit moves. The far end is the game's own.", true},
             {"Button Prompts", "vc3.prompts", "auto",
              {{"Automatic", "auto"}, {"PlayStation", "playstation"}, {"Xbox", "xbox"}, {"Keyboard", "keyboard"}},
              "Which buttons the prompts show. Automatic follows what you last pressed."},
         }},
        // VC3: no Other tab (Discord needs VC3's own application first).
    };
    return t;
}

constexpr std::uint32_t kUp = 0x10u, kRight = 0x20u, kDown = 0x40u, kLeft = 0x80u, kL = 0x100u, kR = 0x200u,
                        kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u;
// Back in the game's menus: Circle, or Cross where Circle confirms (VC3's
// Japanese edition; the English one reports Cross confirms).
std::uint32_t cancel_button() { return portablekit::active_confirm_button() == 0u ? kCross : kCircle; }
// Confirm: the other of the two.
std::uint32_t confirm_button() { return cancel_button() == kCross ? kCircle : kCross; }

// The Settings row under a menu's Options row (entry_frame).
struct Entry {
    int rows = 0, options = -1, focus = -1;
    std::chrono::steady_clock::time_point seen{}, moved{};
    bool on = false;               // our cursor is on Settings (the game's on Options)
    bool returning = false;        // Settings was chosen: the menu comes back on it
    std::uint32_t held = 0u;       // directions and buttons down at the last read
    std::uint32_t swallowed = 0u;  // taken from the game until released
    bool sound_due = false;        // a move of ours, kept from the game, wants its cursor sound
    std::chrono::steady_clock::time_point expect_sound{};  // the game moved its cursor: its sound is next
    std::uint32_t cursor_sound = 0u;  // the game's cursor sound, once heard (0: not yet)
};

struct State {
    std::mutex mutex;
    bool requested = false, active = false;
    std::chrono::steady_clock::time_point requested_at{}, last_shown{};
    int tab = 0, row = 0;
    std::uint32_t held = 0u;
    std::chrono::steady_clock::time_point repeat_at{};
    int back_reads = 0;               // reads that still give the game Back
    std::vector<std::string> changed;  // keys changed since the screen opened
    Entry entry;
    bool stick_taken = false;
};

State &state() {
    static State s;
    return s;
}

std::string format2(float value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", value);
    return text;
}

// The setting's value now, as settings.ini writes it.
std::string current(const Row &row) {
    const std::string key = row.key;
    const settings::Settings &s = settings::current();
    if (key == "vc3.lod") return options::get(options::Key::Lod);
    if (key == "vc3.stick_dead_zone") return options::get(options::Key::StickDeadZone);
    if (key == "vc3.prompts") return options::get(options::Key::Prompts);
    if (key == "vc3.backgrounds") return options::get(options::Key::Backgrounds);
    if (key == "vc3.tick_hz") return options::get(options::Key::TickHz);
    if (key == "vc3.discord") return options::get(options::Key::Discord);
    if (key == "video.display_mode") {
        switch (settings::display_mode(s)) {
        case settings::DisplayMode::Borderless: return "borderless";
        case settings::DisplayMode::Fullscreen: return "fullscreen";
        default: return "window";
        }
    }
    if (key == "video.resolution") return settings::format_resolution(s.resolution);
    if (key == "video.aspect") return settings::format_aspect(s);
    if (key == "video.present_mode")
        return s.present_mode == settings::PresentMode::Mailbox     ? "mailbox"
               : s.present_mode == settings::PresentMode::Immediate ? "immediate"
                                                                    : "vsync";
    if (key == "video.sharp_textures") return s.sharp_textures ? "1" : "0";
    if (key == "video.sharp_screen") return s.sharp_screen ? "1" : "0";
    if (key == "video.performance")
        return s.perf == settings::PerfDisplay::Off || s.perf == settings::PerfDisplay::Log ? "off"
               : s.perf == settings::PerfDisplay::Fps                                       ? "fps"
                                                                                            : "overlay";
    if (key == "video.texture_pack") return s.texture_pack ? "1" : "0";
    if (key == "input.camera_speed") return format2(s.camera_speed);
    if (key == "input.mouse_sensitivity") return format2(s.mouse_sensitivity);
    if (key == "input.invert_camera_y") return s.invert_camera_y ? "1" : "0";
    if (key == "input.invert_camera_x") return s.invert_camera_x ? "1" : "0";
    return row.fallback;
}

int chosen_index(const Row &row) {
    const std::string value = current(row);
    for (std::size_t i = 0; i < row.choices.size(); ++i)
        if (value == row.choices[i].value) return static_cast<int>(i);
    // Numbers written another way ("190" for "190.00").
    char *end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    if (end != value.c_str())
        for (std::size_t i = 0; i < row.choices.size(); ++i)
            if (std::abs(std::strtod(row.choices[i].value, nullptr) - number) < 1e-4) return static_cast<int>(i);
    return -1;
}

// Applies a value now; true when it took.
bool apply(const Row &row, const std::string &value) {
    const std::string key = row.key;
    settings::Settings &s = settings::current();
    if (key == "vc3.lod") {
        if (!options::set(options::Key::Lod, value)) return false;
        lod_apply_live();
        return true;
    }
    if (key == "vc3.stick_dead_zone") return options::set(options::Key::StickDeadZone, value);
    // Read every frame by the widescreen layout (vc3_native_gu.cpp).
    if (key == "vc3.backgrounds") return options::set(options::Key::Backgrounds, value);
    if (key == "vc3.tick_hz") {
        if (options::overridden(options::Key::TickHz) || !set_frame_rate(value)) return false;
        return options::set(options::Key::TickHz, value);
    }
    if (key == "vc3.discord") {
        if (!options::set(options::Key::Discord, value)) return false;
        set_discord(value != "off");
        return true;
    }
    if (key == "vc3.prompts") {
        if (!options::set(options::Key::Prompts, value)) return false;
        prompts_changed();
        return true;
    }
    if (key.rfind("video.", 0) == 0) return apply_video_setting(key, value);
    const float number = std::strtof(value.c_str(), nullptr);
    if (key == "input.camera_speed") s.camera_speed = number;
    else if (key == "input.mouse_sensitivity") s.mouse_sensitivity = number;
    else if (key == "input.invert_camera_y") s.invert_camera_y = value == "1";
    else if (key == "input.invert_camera_x") s.invert_camera_x = value == "1";
    else return false;
    return true;
}

// Writes the changed PortableKit keys (video.*, input.*) to settings.ini; the
// vc3.* keys are written by options::save(). Keys an environment variable
// decides are left as the file has them.
void save(State &st) {
    if (st.changed.empty()) return;
    try {
        const auto directory = install::user_data_directory();
        auto entries = install::read_settings_file(directory);
        bool any = false;
        for (const Tab &tab : tabs())
            for (const Row &row : tab.rows) {
                const std::string key = row.key;
                if (key.rfind("vc3.", 0) == 0) continue;
                if (std::find(st.changed.begin(), st.changed.end(), key) == st.changed.end()) continue;
                if (!settings::overridden_by(row.key).empty()) continue;
                entries[key] = current(row);
                any = true;
            }
        if (any) install::write_settings_file(directory, entries);
    } catch (const std::exception &e) {
        std::cerr << "[vc3-settings-menu] cannot save settings.ini: " << e.what() << "\n";
    }
    if (!options::save()) std::cerr << "[vc3-settings-menu] the vc3.* settings were not saved\n";
    std::cerr << "[vc3-settings-menu] saved " << st.changed.size() << " changed setting(s)\n";
    st.changed.clear();
}

void step_value(State &st, int delta) {
    const Tab &tab = tabs()[st.tab];
    if (st.row < 0 || st.row >= static_cast<int>(tab.rows.size())) return;
    const Row &row = tab.rows[st.row];
    const int count = static_cast<int>(row.choices.size());
    int index = chosen_index(row);
    index = index < 0 ? 0 : std::clamp(index + delta, 0, count - 1);
    if (index == chosen_index(row)) return;
    const std::string value = row.choices[index].value;
    if (!apply(row, value)) {
        std::cerr << "[vc3-settings-menu] " << row.key << "=" << value << " was not applied\n";
        return;
    }
    if (std::find(st.changed.begin(), st.changed.end(), row.key) == st.changed.end()) st.changed.push_back(row.key);
    std::cerr << "[vc3-settings-menu] " << row.key << " = " << value << "\n";
}

void set_default(State &st) {
    const Tab &tab = tabs()[st.tab];
    if (st.row < 0 || st.row >= static_cast<int>(tab.rows.size())) return;
    const Row &row = tab.rows[st.row];
    int target = 0;
    for (std::size_t i = 0; i < row.choices.size(); ++i)
        if (std::string(row.choices[i].value) == row.fallback) target = static_cast<int>(i);
    const int now = chosen_index(row);
    if (now != target) step_value(st, target - std::max(now, 0));
}

void request_locked(State &st) {
    st.requested = true;
    st.requested_at = std::chrono::steady_clock::now();
    std::cerr << "[vc3-settings-menu] SETTINGS chosen: the Options screen opens as the Settings screen\n";
}

// A menu with our Settings row is on screen (entry_frame within 0.3 s).
bool entry_shown(const State &st, std::chrono::steady_clock::time_point now) {
    return st.entry.rows > 0 && now - st.entry.seen < std::chrono::milliseconds(300);
}

// One pad read while such a menu is up. The game's cursor stays on Options
// while ours is on Settings: down from Options (taken from the game) puts it
// on Settings; up from Settings (taken) back on Options; down from Settings
// (given) moves the game's cursor on from Options; up from the row under
// Options (given: the game's cursor goes to Options) lands on Settings.
// Cross on Settings is the game's own Cross on Options, with the Settings
// screen asked for.
std::uint32_t entry_buttons(State &st, std::uint32_t buttons, int stick_x, int stick_y) {
    Entry &e = st.entry;
    const auto now = std::chrono::steady_clock::now();
    std::uint32_t pad = buttons;
    if (stick_y < 128 - 64) pad |= kUp;
    if (stick_y > 128 + 64) pad |= kDown;
    const std::uint32_t pressed = pad & ~e.held;
    e.held = pad;
    e.swallowed &= pad;  // given back once released
    if (!entry_shown(st, now)) {
        e.swallowed = 0u;
        st.stick_taken = false;
        return buttons;
    }
    const auto log = [&](const char *what) {
        static std::uint32_t logged = 0u;
        if (++logged <= 200u) std::cerr << "[vc3-settings-entry] " << what << "\n";
    };
    // Up or down given to the game moves its cursor, with its cursor sound.
    const std::uint32_t moves = pressed & (kUp | kDown);
    if (e.on) {
        if (pressed & kUp) {
            e.on = false, e.moved = now, e.swallowed |= kUp, e.sound_due = true;
            log("up: back on Options");
        } else if (pressed & kDown) {
            e.on = false, e.moved = now;
            log("down: the game's cursor moves on from Options");
        } else if (pressed & confirm_button()) {
            request_locked(st);
            e.returning = true;
        } else if (pressed & cancel_button()) {
            e.on = false;
        }
    } else if (e.focus >= 0 && e.focus == e.options) {
        if (pressed & kDown) {
            e.on = true, e.moved = now, e.swallowed |= kDown, e.sound_due = true;
            log("down: on Settings");
        }
    } else if (e.focus >= 0 && (pressed & kUp) && e.rows > 0 && (e.focus + e.rows - 1) % e.rows == e.options) {
        e.on = true, e.moved = now;
        log("up: on Settings (the game's cursor goes to Options)");
    }
    if ((moves & ~e.swallowed) != 0u) e.expect_sound = now + std::chrono::milliseconds(300);
    st.stick_taken = e.swallowed != 0u;
    return buttons & ~e.swallowed;
}

}  // namespace

void request() {
    State &st = state();
    std::lock_guard lock(st.mutex);
    request_locked(st);
}

void entry_frame(int rows, int options, int focus) {
    State &st = state();
    std::lock_guard lock(st.mutex);
    const auto now = std::chrono::steady_clock::now();
    Entry &e = st.entry;
    if (rows > 0) {
        if (!entry_shown(st, now)) {  // a menu just opened (or came back from the Settings screen)
            e.on = e.on && e.returning;
            e.returning = false;
        }
        e.rows = rows, e.options = options, e.focus = focus, e.seen = now;
        // The game's cursor left Options another way (or never got there, a
        // list that does not wrap): ours follows it.
        if (e.on && focus >= 0 && focus != options && now - e.moved > std::chrono::milliseconds(250)) e.on = false;
    }
}

void sound_played(std::uint32_t id) {
    State &st = state();
    std::lock_guard lock(st.mutex);
    Entry &e = st.entry;
    const auto now = std::chrono::steady_clock::now();
    if (now >= e.expect_sound || id == 0u || id >= 100u) return;
    e.expect_sound = {};
    if (e.cursor_sound != id) std::cerr << "[vc3-settings-entry] the menu's cursor sound is " << id << "\n";
    e.cursor_sound = id;
}

bool take_cursor_sound(std::uint32_t &id) {
    State &st = state();
    std::lock_guard lock(st.mutex);
    Entry &e = st.entry;
    if (!e.sound_due) return false;
    e.sound_due = false;
    id = e.cursor_sound != 0u ? e.cursor_sound : 1u;
    return true;
}

bool entry_on() {
    State &st = state();
    std::lock_guard lock(st.mutex);
    return st.entry.on && entry_shown(st, std::chrono::steady_clock::now());
}

bool stick_taken() {
    State &st = state();
    std::lock_guard lock(st.mutex);
    return st.stick_taken;
}

void options_frame(bool shown) {
    State &st = state();
    std::lock_guard lock(st.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (shown) {
        st.last_shown = now;
        if (st.requested && !st.active) {
            st.active = true;
            st.requested = false;
            st.tab = 0, st.row = 0, st.held = 0u, st.back_reads = 0;
            st.changed.clear();
            std::cerr << "[vc3-settings-menu] open\n";
        }
        return;
    }
    // A request the Options screen never followed (10 s) is dropped.
    if (st.requested && now - st.requested_at > std::chrono::seconds(10)) st.requested = false;
    if (st.active && now - st.last_shown > std::chrono::milliseconds(300)) {
        st.active = false;
        save(st);
        std::cerr << "[vc3-settings-menu] closed\n";
    }
}

bool active() {
    State &st = state();
    std::lock_guard lock(st.mutex);
    return st.active;
}

void give_back() {
    State &st = state();
    std::lock_guard lock(st.mutex);
    if (!st.active) return;
    st.active = false;
    st.requested = false;
    save(st);
    std::cerr << "[vc3-settings-menu] the Settings screen could not be drawn: the game's Options screen keeps its own "
                 "controls\n";
}

std::uint32_t filter_buttons(std::uint32_t buttons, int stick_x, int stick_y) {
    State &st = state();
    std::lock_guard lock(st.mutex);
    if (!st.active) return entry_buttons(st, buttons, stick_x, stick_y);
    st.stick_taken = false;
    st.entry.held = 0xFFFFFFFFu;  // nothing held over counts as a press when the screen closes
    if (st.back_reads > 0) {
        --st.back_reads;
        return st.back_reads % 2 == 1 ? cancel_button() : 0u;  // pressed, released, pressed: one clean Back
    }
    // The left stick counts as the D-pad (W A S D arrive there).
    std::uint32_t pad = buttons;
    if (stick_y < 128 - 64) pad |= kUp;
    if (stick_y > 128 + 64) pad |= kDown;
    if (stick_x < 128 - 64) pad |= kLeft;
    if (stick_x > 128 + 64) pad |= kRight;
    const std::uint32_t pressed = pad & ~st.held;
    const auto now = std::chrono::steady_clock::now();
    std::uint32_t act = pressed;
    // Held up / down / left / right repeat after 0.35 s, every 0.09 s.
    const std::uint32_t repeating = pad & (kUp | kDown | kLeft | kRight);
    if (pressed & repeating) st.repeat_at = now + std::chrono::milliseconds(350);
    else if (repeating != 0u && (pad & st.held & repeating) && now >= st.repeat_at) {
        act |= repeating;
        st.repeat_at = now + std::chrono::milliseconds(90);
    }
    st.held = pad;
    const int rows = static_cast<int>(tabs()[st.tab].rows.size());
    if (act & kUp) st.row = (st.row + rows - 1) % rows;
    if (act & kDown) st.row = (st.row + 1) % rows;
    if (act & kLeft) step_value(st, -1);
    if (act & kRight) step_value(st, +1);
    if (pressed & (kL | kR)) {
        const int count = static_cast<int>(tabs().size());
        st.tab = (st.tab + ((pressed & kR) ? 1 : count - 1)) % count;
        st.row = 0;
    }
    // Triangle, or SELECT as VC3's own hint says ("SELECT: Initialize"): the row's default.
    if (pressed & (kTriangle | 0x1u)) set_default(st);
    if (pressed & cancel_button()) {
        save(st);
        st.back_reads = 3;  // Back for the game's Options screen, which then closes
        std::cerr << "[vc3-settings-menu] back\n";
    }
    return 0u;
}

View view() {
    State &st = state();
    std::lock_guard lock(st.mutex);
    View v;
    v.title = "Settings";
    for (const Tab &tab : tabs()) v.tabs.push_back(tab.name);
    v.tab = st.tab;
    v.focus = st.row;
    const Tab &tab = tabs()[st.tab];
    for (const Row &row : tab.rows) {
        RowView r;
        r.label = row.label;
        const int index = chosen_index(row);
        if (row.slider) {
            r.slider = true;
            const int count = static_cast<int>(row.choices.size());
            r.fraction = count > 1 && index >= 0 ? static_cast<float>(index) / static_cast<float>(count - 1) : 0.0f;
            // As a stepper too, for a frame without the game's slider to copy.
            r.stepper = true;
            r.values.push_back(index >= 0 ? row.choices[index].label : current(row));
            r.first = index <= 0;
            r.last = index == count - 1;
        } else if (row.choices.size() <= 3u) {
            for (const Choice &choice : row.choices) r.values.push_back(choice.label);
            r.selected = index;
        } else {
            r.stepper = true;
            r.values.push_back(index >= 0 ? row.choices[index].label : current(row));
            r.first = index <= 0;
            r.last = index == static_cast<int>(row.choices.size()) - 1;
        }
        v.rows.push_back(std::move(r));
    }
    if (st.row >= 0 && st.row < static_cast<int>(tab.rows.size())) {
        const Row &row = tab.rows[st.row];
        v.help = row.help;
        // A slider shows no number: the description says it.
        if (row.slider) {
            const int index = chosen_index(row);
            v.help += std::string(" Now: ") + (index >= 0 ? row.choices[index].label : current(row).c_str()) + ".";
        }
    }
    return v;
}

}  // namespace portablekit::vc3::settings_menu

// The game's sound effect function was called (cmake/vc3_sites.cmake).
extern "C" void vc3_sound_played(std::uint32_t id) noexcept { portablekit::vc3::settings_menu::sound_played(id); }
