// VC2 controls presets (launcher Controls -> Controls, VC3_CONTROLS).
//
// Classic (PSP): the game's own controls, as on the PSP: L / R and Triangle /
// Cross turn the Action Mode camera, L + R resets it, no right-stick or mouse
// camera. Nothing here changes what the game reads.
//
// Modern (VC4), the default: what makes VC2 play like Valkyria Chronicles 4.
// - The right stick and the mouse turn the Action Mode camera (vc3_camera.cpp),
//   and the game's own button turning is cancelled there, so the shoulders and
//   face buttons are free.
// - In Action Mode (the moved unit's camera code ran during the previous
//   frame; vc3_camera.cpp), the game's pad is remapped:
//     R (RB / RT, R1 / R2)   -> Square: Target Mode (VC4: R1 aims). VC2 Europe
//                               enters Target Mode with Square (seen in the
//                               log 2026-11-08 19:30); Cross held there turns
//                               the camera down instead (the face-button pitch)
//   Circle (B) is left as it is: the game's own Circle crouches behind
//   sandbags when near them and otherwise ends the action (owner, 20:10; an
//   earlier Circle -> START remap lost the crouch).
//     R3, middle mouse       -> L + R: the game's camera reset
//   Cross (A) stays the confirm and terrain-action button, Square stays Target
//   Mode too.
// - Everywhere else (Command Mode, Target Mode, menus, story, the academy map)
//   the game's buttons are left alone: in Target Mode L / R change target,
//   Cross fires and Circle cancels, as VC4 does.
// A button keeps the meaning it had when it was pressed until it is released,
// so the R that enters Target Mode does not also change target on the next
// frame.
//
// The remap is applied where the game reads the pad: VC2 calls only
// sceCtrlPeekBufferPositive (about ten times a frame; never ReadBuffer or the
// latch), and derives presses from those reads itself. ReadBuffer is wrapped
// too in case another release uses it. Europe and the US, like the camera sites.
//
// Keyboard and mouse, Modern: Valkyria Chronicles 4's PC defaults, read from
// VC4's own Key Bindings screen (2026-11-09), mapped to VC2's buttons per mode.
// Host code reads the keys and mouse buttons, so a key can mean different
// things in different modes. PortableKit's own key bindings for the PSP
// buttons (face, shoulders, START, SELECT) are cleared for the run
// (settings::current(), not written to settings.ini); its stick (W A S D),
// D-pad (arrows) and camera keys (I J K L, VC4's "Alt. Camera") stay, and the
// mouse still turns the camera and aims (vc3_camera.cpp).
//   VC4 action (VC4 key)           Action Mode     Target Mode    academy map     elsewhere
//   Action Button (left click)     Cross (act)     Cross (fire)   Cross           Cross (confirm)
//   (menus: Space, Enter)          Cross           Cross          Cross           Cross
//   Toggle aiming (right click)    Square (aim)    Circle (stop)  Circle (back),  Circle (back)
//                                                                 held Triangle *
//   Back / end turn (Esc, Bksp)    Circle (crouch at sandbags, else end; stop aiming; back)
//   Context (E; owner's choice:    Circle          -              -               -
//     easy to reach): the game's own contextual Circle (crouch or crawl at
//     cover, capture a base), so with nothing to act on it ends the action
//     like Esc
//   Select previous / next item    -               -              L / R           L / R
//     (1 / 2)                                      (change weapon: C only, owner's choice 2026-11-11; with
//                                                  1 as well the prompts kept switching between the two)
//   Lock left / right target       -               L / R          -               -
//     (A / D while aiming; W A S D then do not move the reticle)
//   Reset camera (R)               L + R           -              -               -
//   Triangle (Q; owner's choice,   Triangle        Triangle       Triangle        Triangle
//     2026-11-09: VC4's Peep left, unused here, next to W A S D)
//   Square (C; owner's choice,     Square          Square         Square          Square; held,
//     2026-11-09: free, pairs                                                     the mouse drives
//     with Q)                                                                     the stick (R&D)
//   Open interface (F)             SELECT (menu)   SELECT         SELECT          SELECT
//   End turn instant (X)           START           -              START           START (end phase)
//   Walk (hold Shift)              W A S D are the d-pad (the game's slow walk), the stick rests
//   * In menus W A S D are the d-pad (some read only it: the shop's class
//     tabs), not the stick.
//   * C held in a menu (R&D's vehicle view): W A S D are also the d-pad, which
//     the view's prompts show (zoom, turn); the mouse still drives the stick.
//   * right button held on the academy map: Triangle (the map camera), and the
//     mouse turns it instead of moving the cursor; W A S D still move the
//     cursor between places.
//   No VC2 equivalent, left unbound: Peep left (Q), Toggle sightlines (C),
//   Toggle minimap (Z), shoulder cameras while aiming (Q / E).
// Keys are read only while the game's window has the keyboard, mouse buttons
// only while it has the pointer captured. A key keeps the meaning it had when
// pressed until released. These are added after the gamepad's Action Mode
// remap, so they are never remapped again.
//
// Continue (launcher: Continue, VC3_CONTINUE=1): from boot straight into the
// last save, by pressing what the owner presses (log of 2026-11-08 19:53):
// START and Cross in turn through the logos, the title and the main menu
// until the game opens its load list (its second sceUtilitySavedata FILES
// request: the first is at boot), then Cross on the slot (the game puts the
// cursor on the last one used) and Cross on the confirmation. Any button the
// player presses stops it, and it gives up after two minutes.
#include "vc3_controls.hpp"
#include "vc3_camera.hpp"
#include "vc3_discord.hpp"
#include "vc3_region.hpp"
#include "vc3_settings.hpp"
#include "vc3_settings_menu.hpp"

#include "profile.hpp"
#include "kernel/kernel.hpp"
#include "input/bindings.hpp"
#include "settings/settings.hpp"

#if defined(PORTABLEKIT_HAS_SDL)
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace portablekit::vc3 {
namespace {

constexpr std::uint32_t kSelect = 0x0001u;
constexpr std::uint32_t kStart = 0x0008u;
constexpr std::uint32_t kTriangle = 0x1000u;
constexpr std::uint32_t kL = 0x0100u;
constexpr std::uint32_t kR = 0x0200u;
constexpr std::uint32_t kCircle = 0x2000u;
constexpr std::uint32_t kCross = 0x4000u;
constexpr std::uint32_t kSquare = 0x8000u;

constexpr std::uint32_t kPeekPositiveNid = 0x3A622550u;
constexpr std::uint32_t kSavedataInitStartNid = 0x50C4CD57u;
constexpr std::uint32_t kSavedataModeFiles = 12u;
constexpr std::uint32_t kReadPositiveNid = 0x1F803938u;

// One input that can mean something else in Action Mode.
struct Source {
    const char *name;
    std::uint32_t bit;      // the game's button it arrives as (0: a host-only button)
    std::uint32_t modern;   // what it becomes in Action Mode
    bool held = false;
    std::uint32_t out = 0u;  // what it means until it is released
};

struct ControlsState {
    Source r{"R", kR, kSquare};
    Source reset{"R3 / middle mouse", 0u, kL | kR};
    std::uint64_t remaps = 0u;
    std::uint32_t last_in = 0u, last_out = 0u;
    std::uint64_t reads = 0u, logged = 0u;
};

// --- Keyboard and mouse (Modern) ------------------------------------------------
enum Context { kInAction, kInTarget, kOnMap, kElsewhere };

struct KbmButton {
    const char *name;
    int keys[2];        // SDL scancodes (0: none)
    int mouse;          // SDL mouse button (0: none)
    std::uint32_t out[4];  // by Context
    const char *cap;       // its button prompt: a key cap's label (null: the mouse button's picture)
    bool held = false;
    std::uint32_t latched = 0u;
    Context pressed_in = kElsewhere;
};

// SDL scancodes (USB HID usages) and mouse buttons, spelled out so this reads
// the same without SDL's headers.
constexpr int kKeyA = 4, kKeyC = 6, kKeyD = 7, kKeyE = 8, kKeyF = 9, kKeyQ = 20, kKeyR = 21, kKeyS = 22, kKeyW = 26, kKeyX = 27, kKey1 = 30,
              kKey2 = 31, kKeyEnter = 40, kKeyEscape = 41, kKeyBackspace = 42, kKeySpace = 44, kKeyLeftShift = 225,
              kKeyRightShift = 229;
constexpr int kMouseLeft = 1, kMouseRight = 3;
constexpr std::size_t kRightClick = 1u;  // indices in kbm_buttons()
constexpr std::size_t kSquareKey = 7u;

// Where the console says Circle confirms (VC3's Japanese edition: the
// profile's confirm_button 0; the English edition reports Cross), the menus
// decide with Circle and cancel with Cross, so there the keys' Cross and
// Circle trade places: the left click and Space still confirm, Esc still goes
// back. Only in menus and on the map: VC3's setting changes only its menus'
// decide / cancel (its pad manager's flag +0x6C, 0x08BB3410), and battle
// actions keep their PSP buttons (owner, 2026-11-15: with everything swapped,
// E no longer did the Circle context action and the left click did).
// VC3_JP_SWAP_CONFIRM=0 keeps the keys as they are.
void swap_confirm_for_japan(std::array<KbmButton, 14> &buttons) {
    if (portablekit::active_confirm_button() != 0u) return;  // Cross confirms: nothing to trade
    if (const char *keep = env("JP_SWAP_CONFIRM"); keep != nullptr && std::string(keep) == "0") return;
    for (KbmButton &button : buttons)
        for (const Context context : {kOnMap, kElsewhere}) {
            std::uint32_t &out = button.out[context];
            const std::uint32_t cross = out & kCross, circle = out & kCircle;
            out = (out & ~(kCross | kCircle)) | (cross != 0u ? kCircle : 0u) | (circle != 0u ? kCross : 0u);
        }
    std::cerr << "[vc3-controls] Circle confirms (Japanese edition): the keys' Cross and Circle swapped in menus "
                 "(battle keys unchanged; VC3_JP_SWAP_CONFIRM=0 keeps them)\n";
}

std::array<KbmButton, 14> &kbm_buttons() {
    static std::array<KbmButton, 14> buttons{{
        {"left click", {0, 0}, kMouseLeft, {kCross, kCross, kCross, kCross}, nullptr},
        // On the map a held right button is Triangle (the map camera) and a
        // quick click goes back (Circle; kbm_buttons_now): Base Camp and the
        // menus over it run the map's screen cursor, where it was Triangle and
        // did not back out (owner, 2026-11-16). On the battle map (Command
        // Mode, the same cursor) it is Triangle at once, the unit's command
        // list, as the tutorial says (owner, 2026-11-23).
        {"right click", {0, 0}, kMouseRight, {kSquare, kCircle, kTriangle, kCircle}, nullptr},
        // After the right button: on the map its prompt stays the right click.
        {"Q", {kKeyQ, 0}, 0, {kTriangle, kTriangle, kTriangle, kTriangle}, "Q"},
        {"Space / Enter", {kKeySpace, kKeyEnter}, 0, {kCross, kCross, kCross, kCross}, "Space"},
        // Before Esc: in Action Mode its prompt is E, the context button.
        {"E (context)", {kKeyE, 0}, 0, {kCircle, 0u, 0u, 0u}, "E"},
        {"Esc / Backspace", {kKeyEscape, kKeyBackspace}, 0, {kCircle, kCircle, kCircle, kCircle}, "Esc"},
        {"1", {kKey1, 0}, 0, {0u, 0u, kL, kL}, "1"},
        // After the right button: in Action Mode the prompt for Square stays
        // the right click (aim); in Target Mode it is C (change weapon). Held
        // in menus, the mouse drives the stick (R&D's "Control" spins the
        // vehicle while Square is held).
        {"C", {kKeyC, 0}, 0, {kSquare, kSquare, kSquare, kSquare}, "C"},
        {"2", {kKey2, 0}, 0, {0u, 0u, kR, kR}, "2"},
        {"A (aiming)", {kKeyA, 0}, 0, {0u, kL, 0u, 0u}, "A"},
        {"D (aiming)", {kKeyD, 0}, 0, {0u, kR, 0u, 0u}, "D"},
        {"R", {kKeyR, 0}, 0, {kL | kR, 0u, 0u, 0u}, "R"},
        {"F", {kKeyF, 0}, 0, {kSelect, kSelect, kSelect, kSelect}, "F"},
        {"X", {kKeyX, 0}, 0, {kStart, 0u, kStart, kStart}, "X"},
    }};
    static const bool swapped = (swap_confirm_for_japan(buttons), true);
    (void)swapped;
    return buttons;
}

// Keyboard state for the stick keys. Walking is Shift held: W A S D then press
// the d-pad, the game's slow walk, instead of the stick (a half-tilted stick
// did not walk: owner 2026-11-09 03:10; Shift + W A S D as the d-pad, owner
// 2026-11-11). C held in a menu does the same (R&D's vehicle view zooms and
// turns on the d-pad, as its prompts show).
struct KeyboardStick {
    bool moving = false;    // W A S D held
    bool walking = false;   // Shift held
    bool square_menu = false;  // C held, pressed in a menu
    std::uint32_t dpad = 0u;   // the d-pad W A S D give (up 0x10, right 0x20, down 0x40, left 0x80)
};

KeyboardStick &keyboard_stick() {
    static KeyboardStick k;
    return k;
}

// PortableKit's own key and mouse bindings for the PSP buttons would press
// them a second time; cleared once, for this run only.
void clear_portablekit_button_bindings() {
    static bool done = false;
    if (done) return;
    done = true;
    using input::Action;
    auto &bindings = settings::current().bindings;
    for (const Action action : {Action::Triangle, Action::Circle, Action::Cross, Action::Square, Action::L, Action::R,
                                Action::Start, Action::Select})
        bindings[static_cast<std::size_t>(action)] = input::Slots{input::kNone, input::kNone};
    std::cerr << "[vc3-controls] keyboard and mouse: VC4 defaults (left click act / fire, right click toggles "
                 "aiming, Esc back / end / crouch, 1 / 2 previous / next (menus, map), C change weapon, A / D lock target while aiming, R reset "
                 "camera, F interface, X end turn instant, Q Triangle, C Square; the mouse drives the stick while the right button "
                 "is held on the academy map or C in menus)\n";
}

// The analog bytes (x | y << 8) the game was last given.
std::atomic<std::uint32_t> &last_analog_word() {
    static std::atomic<std::uint32_t> word{0x8080u};
    return word;
}

// The context of the last pad read, for the button prompts (vc3_glyphs.cpp).
std::atomic<int> &last_context() {
    static std::atomic<int> context{kElsewhere};
    return context;
}

// The right button went down on the battle map (Command Mode).
bool &battle_click() {
    static bool on = false;
    return on;
}

std::uint32_t kbm_buttons_now(Context context) {
#if defined(PORTABLEKIT_HAS_SDL)
    const bool keyboard = SDL_GetKeyboardFocus() != nullptr;
    // Clicks count while the game has the pointer captured, as PortableKit's
    // own bindings did (a click that only focuses the window is not a press).
    // The captured window is the one with the keyboard when the hidden pointer
    // has strayed off it (windowed mode: over the title bar or another
    // screen), where the mouse focus is no window and clicks were lost.
    SDL_Window *pointer = SDL_GetMouseFocus();
    if (pointer == nullptr || !SDL_GetWindowRelativeMouseMode(pointer)) pointer = SDL_GetKeyboardFocus();
    const bool mouse = pointer != nullptr && SDL_GetWindowRelativeMouseMode(pointer);
    int count = 0;
    const bool *keys = SDL_GetKeyboardState(&count);
    const SDL_MouseButtonFlags clicks = mouse ? SDL_GetMouseState(nullptr, nullptr) : 0u;
    std::uint32_t out = 0u;
    for (KbmButton &button : kbm_buttons()) {
        bool held = button.mouse != 0 && (clicks & SDL_BUTTON_MASK(button.mouse)) != 0u;
        for (const int key : button.keys)
            if (!held && key != 0 && keyboard && keys != nullptr && key < count && keys[key]) held = true;
        if (held && !button.held) {
            button.latched = button.out[context];
            button.pressed_in = context;
        }
        static std::uint32_t presses_logged = 0u;
        if (held && !button.held && ++presses_logged <= 5000u)
            std::cerr << "[vc3-controls] " << button.name << " in context " << static_cast<int>(context) << " -> 0x"
                      << std::hex << button.latched << std::dec << "\n";
        // The right button on the battle map: Triangle while held (a unit's
        // command list), no map camera.
        if (&button == &kbm_buttons()[kRightClick] && held && !button.held) battle_click() = button.pressed_in == kOnMap && in_battle();
        if (&button == &kbm_buttons()[kRightClick] && battle_click()) {
            static std::uint32_t battle_logged = 0u;
            if (held && !button.held && ++battle_logged <= 200u)
                std::cerr << "[vc3-controls] right click on the battle map: Triangle (command list)\n";
            button.held = held;
            if (held) out |= kTriangle;
            continue;
        }
        // The right button on the map: Triangle only once held a moment (the
        // map camera); let go sooner, a short Circle (back).
        if (&button == &kbm_buttons()[kRightClick] && button.pressed_in == kOnMap && button.latched == kTriangle) {
            using clock = std::chrono::steady_clock;
            static clock::time_point down{}, back_until{};
            const clock::time_point now = clock::now();
            if (held && !button.held) down = now;
            const bool long_hold = now - down >= std::chrono::milliseconds(200);
            if (!held && button.held && !long_hold) {
                back_until = now + std::chrono::milliseconds(100);
                static std::uint32_t quick_logged = 0u;
                if (++quick_logged <= 200u) std::cerr << "[vc3-controls] right click on the map: quick, Circle (back)\n";
            }
            button.held = held;
            if (held && long_hold) out |= kTriangle;
            if (now < back_until) out |= kCircle;
            continue;
        }
        button.held = held;
        if (held) out |= button.latched;
    }
    // The mouse drives the stick while the right button is held on the academy
    // map (Triangle: its camera) or C in menus (Square: R&D's Control spins the
    // vehicle).
    const KbmButton &right = kbm_buttons()[kRightClick];
    const KbmButton &square = kbm_buttons()[kSquareKey];
    const bool map_hold = right.held && right.pressed_in == kOnMap && !battle_click();
    const bool menu_hold = square.held && square.pressed_in == kElsewhere;
    static bool was_hold = false;
    if ((map_hold || menu_hold) != was_hold) {
        was_hold = map_hold || menu_hold;
        std::cerr << "[vc3-controls] mouse drives the stick: "
                  << (map_hold ? "on (right button, academy map)" : menu_hold ? "on (C, menu)" : "off") << "\n";
    }
    set_map_camera_hold(map_hold || menu_hold);
    // The stick keys (Target Mode: they do not move the reticle).
    KeyboardStick &stick = keyboard_stick();
    const auto key_held = [&](int key) { return keyboard && keys != nullptr && key < count && keys[key]; };
    stick.moving = key_held(kKeyW) || key_held(kKeyA) || key_held(kKeyS) || key_held(kKeyD);
    stick.walking = key_held(kKeyLeftShift) || key_held(kKeyRightShift);
    stick.square_menu = menu_hold;
    stick.dpad = (key_held(kKeyW) ? 0x10u : 0u) | (key_held(kKeyD) ? 0x20u : 0u) | (key_held(kKeyS) ? 0x40u : 0u) |
                 (key_held(kKeyA) ? 0x80u : 0u);
    return out;
#else
    (void)context;
    return 0u;
#endif
}

// --- Continue -------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

struct Continue {
    enum class Phase { Off, ToList, OnList, Done } phase = Phase::Off;
    Clock::time_point start{}, list_at{};
    std::uint32_t files_requests = 0u, list_presses = 0u;
};

Continue &continuing() {
    static Continue c;
    return c;
}

bool continue_wanted() {
    static const bool on = [] {
        const char *text = env("CONTINUE");
        return text != nullptr && *text != '\0' && std::string(text) != "0";
    }();
    return on;
}

void stop_continue(const char *why) {
    Continue &c = continuing();
    if (c.phase == Continue::Phase::Done || c.phase == Continue::Phase::Off) return;
    c.phase = Continue::Phase::Done;
    std::cerr << "[vc3-continue] " << why << "\n";
}

// The buttons the macro holds on this read (0: none). `player` is what the
// player is pressing.
std::uint32_t continue_buttons(std::uint32_t player) {
    Continue &c = continuing();
    if (c.phase == Continue::Phase::Off || c.phase == Continue::Phase::Done) return 0u;
    const auto now = Clock::now();
    if (c.start == Clock::time_point{}) c.start = now;
    if (player != 0u) {
        stop_continue("stopped: the player pressed a button");
        return 0u;
    }
    const double seconds = std::chrono::duration<double>(now - c.start).count();
    if (seconds > 120.0) {
        stop_continue("gave up after two minutes without reaching the save");
        return 0u;
    }
    if (c.phase == Continue::Phase::ToList) {
        // A press of 0.12 s every 0.3 s, START and Cross in turn.
        const auto tick = static_cast<std::uint64_t>(seconds / 0.3);
        const double into = seconds - static_cast<double>(tick) * 0.3;
        if (into >= 0.12) return 0u;
        return tick % 2u == 0u ? kStart : kCross;
    }
    // On the load list: Cross 1.2 s after it opened (the slot), again 1.2 s
    // later (the confirmation), then done.
    const double since = std::chrono::duration<double>(now - c.list_at).count();
    const double press_at = 1.2 * (c.list_presses + 1u);
    if (since >= press_at && since < press_at + 0.12) return kCross;
    if (since >= press_at + 0.12) {
        if (++c.list_presses >= 2u) stop_continue("done: loaded the slot the load list opened on");
    }
    return 0u;
}

void note_savedata_request(Runtime &rt, std::uint32_t params) {
    Continue &c = continuing();
    if (c.phase != Continue::Phase::ToList || params == 0u) return;
    if (rt.memory().load32(params + 0x30u) != kSavedataModeFiles) return;
    if (++c.files_requests < 2u) return;
    c.phase = Continue::Phase::OnList;
    c.list_at = Clock::now();
    std::cerr << "[vc3-continue] the load list is open; choosing the last slot used\n";
}

ControlsState &state() {
    static ControlsState s;
    return s;
}

// R3 on any open gamepad: not a PSP button, so read from SDL directly
// (PortableKit has the gamepads open; this only looks). The keyboard's camera
// reset is R (the table above).
bool reset_held() {
#if defined(PORTABLEKIT_HAS_SDL)
    bool held = false;
    int count = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count && !held; ++i)
            if (SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]))
                held = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        SDL_free(ids);
    }
    return held;
#else
    return false;
#endif
}

// The left stick as the controller reports it, read here from SDL and not
// through PortableKit (its dead zone and rescale): the pad pushed furthest,
// each axis -1..1, y positive down as on the PSP. False with no pad open.
bool own_left_stick(float &x, float &y) {
    x = y = 0.0f;
#if defined(PORTABLEKIT_HAS_SDL)
    bool any = false;
    float best = -1.0f;
    int count = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
            if (pad == nullptr) continue;
            any = true;
            const float px = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            const float py = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            if (px * px + py * py > best) best = px * px + py * py, x = px, y = py;
        }
        SDL_free(ids);
    }
    x = std::clamp(x, -1.0f, 1.0f), y = std::clamp(y, -1.0f, 1.0f);
    return any;
#else
    return false;
#endif
}

// Updates a source from this read and returns what it adds to the buttons.
std::uint32_t update(Source &source, bool held, bool action) {
    if (held && !source.held) {
        source.out = action ? source.modern : source.bit;
        if (action && source.out != source.bit) {
            ControlsState &s = state();
            if (++s.remaps <= 40u || s.remaps % 200u == 0u)
                std::cerr << "[vc3-controls] Action Mode: " << source.name << " -> 0x" << std::hex << source.out
                          << std::dec << " (" << s.remaps << " remapped presses)\n";
        }
    }
    source.held = held;
    return held ? source.out : 0u;
}

// VC3_STICK_DEAD_ZONE: how far the left stick must be pushed to move, as a
// round dead zone on the stick itself (read by own_left_stick, not through
// PortableKit: owner 12:55, "don't rely on PortableKit"; its dead zone covers
// both sticks, and zeroing it drifted the camera, 12:54). "original" (the
// default): the size the PSP game ends up with on a PC pad, PortableKit's 15%
// plus the game's 0.38 band of the rest (0.47). A percent (0..40): that much
// of the stick. "square": the game's own square band, untouched (-1).
float stick_dead_zone() {
    // Worked out again when the setting changes (the in-game Settings screen).
    static std::string chosen;
    static float value = 0.0f;
    const std::string &name = options::get(options::Key::StickDeadZone);
    if (name == chosen && !chosen.empty()) return value;
    chosen = name.empty() ? std::string("original") : name;
    value = [&] {
        if (name == "square") {
            std::cerr << "[vc3-controls] stick dead zone: the game's own (square)\n";
            return -1.0f;
        }
        char *end = nullptr;
        const float percent = name.empty() ? -1.0f : std::strtof(name.c_str(), &end);
        if (name.empty() || name == "original" || end == name.c_str() || percent < 0.0f || percent > 40.0f) {
            std::cerr << "[vc3-controls] stick dead zone: the original size (47%), round (moving)\n";
            return 0.15f + 0.85f * (48.0f / 127.0f);
        }
        std::cerr << "[vc3-controls] stick dead zone " << percent << "%, round (moving)\n";
        return percent / 100.0f;
    }();
    return value;
}

void remap_inner(Runtime &rt, std::uint32_t buffer, std::uint32_t count) {
    if (buffer == 0u || count == 0u) return;
    ControlsState &s = state();
    auto &memory = rt.memory();
    const bool modern = controls_preset() == ControlsPreset::Modern;
    const bool action = modern && action_mode();
    // The newest sample decides what is held; every sample read gets the same.
    const std::uint32_t newest = memory.load32(buffer + (count - 1u) * 16u + 4u);
    const std::uint32_t macro = continue_buttons(newest);
    std::uint32_t added = macro;
    std::uint32_t replaced = 0u;
    bool in_menu = false;  // the context is "elsewhere" (menus)
    if (modern) {
        added |= update(s.r, (newest & kR) != 0u, action);
        added |= update(s.reset, reset_held(), action);
        replaced = kR;
        // Holding the right button on the map stops the map's cursor code (the
        // camera takes over), but it is still the map.
        const KbmButton &right = kbm_buttons()[kRightClick];
        const bool map_held = right.held && right.pressed_in == kOnMap && !battle_click();
        const Context context = action                       ? kInAction
                                : target_mode()              ? kInTarget
                                : cursor_mode() || map_held  ? kOnMap
                                                             : kElsewhere;
        last_context().store(static_cast<int>(context), std::memory_order_relaxed);
        in_menu = context == kElsewhere;
        // Each change of context, for the KBM lock-up after an enemy turn
        // (owner, 2026-11-15, online co-op): which context the keys were in.
        static int logged_context = -1;
        static std::uint32_t context_lines = 0u;
        if (static_cast<int>(context) != logged_context && ++context_lines <= 5000u) {
            logged_context = static_cast<int>(context);
            static const char *names[] = {"Action Mode", "Target Mode", "academy map", "elsewhere"};
            std::cerr << "[vc3-controls] context " << logged_context << " ("
                      << (logged_context >= 0 && logged_context < 4 ? names[logged_context] : "?") << ")\n";
        }
        added |= kbm_buttons_now(context);
    }
    // W A S D as the d-pad: walking (Shift, Action Mode; the stick then
    // rests) and C held in a menu (the vehicle view; the mouse's stick stays).
    const KeyboardStick &stick = keyboard_stick();
    const bool walk = modern && action && stick.walking && stick.moving;
    // In menus W A S D are the d-pad, not the stick: some read only the d-pad
    // (the procurement shop's class tabs did not move with A / D, owner
    // 2026-11-16), and every list takes either.
    const bool menu_dpad = modern && in_menu && !stick.square_menu;
    if (modern && stick.moving && (walk || stick.square_menu || menu_dpad)) {
        added |= stick.dpad;
        static std::uint32_t logged = 0u;
        if (++logged <= 40u)
            std::cerr << "[vc3-controls] W A S D as the d-pad ("
                      << (walk ? "walking, Shift" : stick.square_menu ? "C held in a menu" : "in a menu")
                      << "): 0x" << std::hex << stick.dpad << std::dec << "\n";
    }
    // The keyboard stick: while aiming, A / D lock targets and W A S D do not
    // move the reticle (VC4). Walking, or as the d-pad in a menu, they do not
    // push the stick either.
    if (modern && stick.moving && (target_mode() || walk || menu_dpad)) {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t at = buffer + i * 16u + 8u;
            memory.store32(at, (memory.load32(at) & 0xFFFF0000u) | 128u | (128u << 8u));
        }
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t at = buffer + i * 16u + 4u;
        const std::uint32_t buttons = memory.load32(at);
        memory.store32(at, (buttons & ~replaced) | added);
    }
    // The map camera from the mouse (right button held on the academy map):
    // the nub follows the mouse, W A S D aside.
    float map_x = 0.0f, map_y = 0.0f;
    if (modern && map_camera_stick(map_x, map_y)) {
        const auto axis = [](float v) {
            return static_cast<std::uint32_t>(std::clamp(128 + static_cast<int>(std::lround(v * 127.0f)), 0, 255));
        };
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t at = buffer + i * 16u + 8u;
            const std::uint32_t analog = memory.load32(at);
            memory.store32(at, (analog & 0xFFFF0000u) | axis(map_x) | (axis(map_y) << 8u));
        }
    }
    {
        const std::uint32_t analog = memory.load32(buffer + (count - 1u) * 16u + 8u);
        const std::uint32_t pressed = memory.load32(buffer + (count - 1u) * 16u + 4u);
        last_analog_word().store((analog & 0xFFFFu) | ((pressed & 0xF0u) << 16u), std::memory_order_relaxed);
    }
    // Stick dead zone for moving (Action Mode). The game's own dead zone is
    // round, about 48 of 127 of the push's length (measured 03:40 with the
    // [vc3-move] trace: offset 47 -> 0, 62 -> 0.2, 91 -> 0.6, 114 -> 0.8; an
    // earlier note here called it square per axis, which was wrong). The left
    // stick is read from the controller itself (own_left_stick), the dead zone
    // is the player's (round), and the game is given the push's own direction
    // with its length moved past the game's band, so every angle walks true. Moving
    // has two speeds anyway, stopped and running; the D-pad is the slow walk.
    // The size: stick_dead_zone() (launcher Controls > Stick dead zone).
    // Menus, the map and aiming keep the game's band.
    if (const float dead_zone = stick_dead_zone(); dead_zone >= 0.0f && action_mode()) {
        float px = 0.0f, py = 0.0f;
        const bool pad = own_left_stick(px, py);
        const float push = std::min(1.0f, std::sqrt(px * px + py * py));
        // The keyboard's stick (W A S D, either preset) arrives in PortableKit's
        // analog at full tilt: kept as it is while the pad's stick rests.
        const std::uint32_t given = memory.load32(buffer + (count - 1u) * 16u + 8u);
        const int gx = static_cast<int>(given & 0xFFu) - 128, gy = static_cast<int>((given >> 8u) & 0xFFu) - 128;
        const bool keys = push <= dead_zone && gx * gx + gy * gy >= 100 * 100;
        if (pad && !keys) {
            std::uint32_t word = 128u | (128u << 8u);
            if (push > dead_zone) {
                // The game's own dead zone is round (its length, not each axis:
                // 03:40 trace, (84, 35) -> (0.5, -0.2)), so the push's direction
                // goes through untouched; only its length is moved past the
                // game's band: band + speed x the rest. (Building each axis on
                // its own bent every angle toward a diagonal, owner 12:59.)
                // Moving has two speeds, stopped and running (owner), and the
                // game only starts at about half its scale (12:36-12:40 traces:
                // 0.3-0.4 stood still, 0.5 walked): past the player's dead zone
                // the push is given at full reach, so it runs as soon as the
                // dead zone is passed (13:03: a 5% setting changed nothing while
                // small pushes came through below that start).
                const float length = std::sqrt(px * px + py * py);
                const float reach = 127.0f;
                const auto axis = [&](float d) {
                    return std::clamp(static_cast<int>(std::lround(128.0f + d / length * reach)), 0, 255);
                };
                word = static_cast<std::uint32_t>(axis(px)) | (static_cast<std::uint32_t>(axis(py)) << 8u);
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t at = buffer + i * 16u + 8u;
                memory.store32(at, (memory.load32(at) & 0xFFFF0000u) | word);
            }
        }
    }
    if (!modern) return;
    // Diagnostic (first 300 changes): what the game was given, and in which mode.
    ++s.reads;
    const std::uint32_t out = (newest & ~replaced) | added;
    if ((newest != s.last_in || out != s.last_out) && ++s.logged <= 300u)
        std::cerr << "[vc3-controls] read " << s.reads << (action ? " Action Mode" : " other") << ": buttons 0x" << std::hex
                  << newest << " -> 0x" << out << std::dec << " (" << count << " sample" << (count == 1u ? "" : "s")
                  << ", buffer 0x" << std::hex << buffer << std::dec << ")\n";
    s.last_in = newest;
    s.last_out = out;
}

// Every pad read: the presets above, then the in-game Settings screen, which
// while it is open takes the buttons (and the left stick) for itself.
// The game's sound effect function (VC3's own address: the English edition
// runs the same code).
constexpr std::uint32_t kPlaySound = 0x08816500u;

void remap(Runtime &rt, std::uint32_t buffer, std::uint32_t count) {
    remap_inner(rt, buffer, count);
    if (buffer == 0u || count == 0u) return;
    auto &memory = rt.memory();
    const std::uint32_t newest = memory.load32(buffer + (count - 1u) * 16u + 4u);
    const std::uint32_t analog = memory.load32(buffer + (count - 1u) * 16u + 8u);
    const std::uint32_t given = settings_menu::filter_buttons(newest, static_cast<int>(analog & 0xFFu),
                                                              static_cast<int>((analog >> 8u) & 0xFFu));
    // The stick too, while the screen is open or a direction the menu took
    // for itself (the Settings row under Options) is held.
    const bool centre = settings_menu::active() || settings_menu::stick_taken();
    if (given == newest && !centre) return;
    for (std::uint32_t i = 0; i < count; ++i) {
        memory.store32(buffer + i * 16u + 4u, given);
        const std::uint32_t at = buffer + i * 16u + 8u;
        if (centre) memory.store32(at, (memory.load32(at) & 0xFFFF0000u) | 128u | (128u << 8u));
    }
}

}  // namespace

int controls_context() { return last_context().load(std::memory_order_relaxed); }

void last_analog(int &x, int &y, int &dpad) {
    const std::uint32_t word = last_analog_word().load(std::memory_order_relaxed);
    x = static_cast<int>(word & 0xFFu);
    y = static_cast<int>((word >> 8u) & 0xFFu);
    dpad = static_cast<int>((word >> 16u) & 0xF0u);
}

bool kbm_prompt(std::uint32_t psp_button, KbmPrompt &out) { return kbm_prompt_in(psp_button, controls_context(), out); }

bool kbm_prompt_in(std::uint32_t psp_button, int context, KbmPrompt &out) {
    if (controls_preset() != ControlsPreset::Modern || context < 0 || context > 3) return false;
    for (const KbmButton &button : kbm_buttons()) {
        if (button.out[context] != psp_button) continue;
        out.mouse = button.cap == nullptr ? button.mouse : 0;
        out.cap = button.cap;
        return true;
    }
    return false;
}

ControlsPreset controls_preset() {
    static const ControlsPreset preset = [] {
        // VC3_CONTROLS (launcher: Controls > Controls preset), Modern by
        // default now that VC3's camera, aim and cursor sites are patched.
        const char *text = options::get(options::Key::Controls).c_str();
        const bool classic = text != nullptr && std::string(text) == "classic";
        return classic ? ControlsPreset::Classic : ControlsPreset::Modern;
    }();
    return preset;
}

void register_controls(HleRegistrar &hle) {
    const bool modern = controls_preset() == ControlsPreset::Modern;
    std::cerr << "[vc3-controls] " << (modern ? "Modern (VC4)" : "Classic (PSP)") << " controls"
              << (modern ? ": right stick / mouse camera, RB aims, R3 resets the camera"
                         : ": the game's own")
              << "\n";
    if (continue_wanted()) {
        continuing().phase = Continue::Phase::ToList;
        std::cerr << "[vc3-continue] continuing from the last save: START and Cross until the load list\n";
        if (HleFunction inner = hle.bound_function("sceUtility", kSavedataInitStartNid)) {
            hle.bind("sceUtility", kSavedataInitStartNid,
                     [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
                         note_savedata_request(rt, arg(ctx, 0u));
                         inner(rt, ctx);
                     });
        } else {
            std::cerr << "[vc3-continue] sceUtilitySavedataInitStart is not bound; continue off\n";
            continuing().phase = Continue::Phase::Off;
        }
    }
    if (modern) clear_portablekit_button_bindings();
    if (!modern && !continue_wanted() && stick_dead_zone() < 0.0f) return;  // nothing to change
    for (const std::uint32_t nid : {kPeekPositiveNid, kReadPositiveNid}) {
        HleFunction inner = hle.bound_function("sceCtrl", nid);
        if (!inner) continue;
        hle.bind("sceCtrl", nid, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            const std::uint32_t buffer = arg(ctx, 0u);
            const std::uint32_t asked = std::clamp<std::uint32_t>(arg(ctx, 1u), 1u, 64u);
            inner(rt, ctx);
            const auto got = static_cast<std::int32_t>(ctx.gpr[2]);
            if (got <= 0) return;
            remap(rt, buffer, std::min(asked, static_cast<std::uint32_t>(got)));
            // A cursor move the Settings row kept from the game: the game's own
            // cursor sound, played by its sound function (VC3 0x08816500: a0
            // the sound, a1 0, f12 the volume, f13 the pan) as the game calls
            // it, then the read returns as it would have.
            std::uint32_t sound = 0u;
            if (settings_menu::take_cursor_sound(sound)) {
                const auto result = static_cast<std::uint32_t>(got);
                ctx.fpr[12] = 1.0f;
                ctx.fpr[13] = 0.0f;
                portablekit::kernel().call_guest(ctx, kPlaySound, portablekit::GuestCallArguments{sound, 0u, 0u, 0u},
                                    [result](AllegrexContext &c, std::uint32_t) { portablekit::kernel().finish(c, result); });
            }
        });
    }
}

}  // namespace portablekit::vc3
