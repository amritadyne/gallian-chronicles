#pragma once

#include "input/bindings.hpp"
#include "input/tilt.hpp"

#include <cstdint>
#include <string>
#include <vector>

// The player's settings: what the in-game menu changes and settings.ini in the
// per-user data directory keeps.
//
// A value comes from its environment variable when that is set, otherwise from
// settings.ini, otherwise from the default below. A variable decides the value
// for the whole run: the menu shows it but cannot change it, and it is never
// written to settings.ini, so unsetting the variable brings the player's own
// choice back.
//
// Only the main thread reads or writes these.
namespace portablekit::settings {

// The profile's default player name (GameProfile::default_player_name), for
// the fixed name before the player sets one.
[[nodiscard]] std::string game_default_name();

enum class PresentMode { Fifo, Mailbox, Immediate };
// How the game's picture meets the window. Original keeps the PSP's shape with
// black bars, Stretch fills the window by stretching it, and Fill gives the
// game the window's shape: its view widens or narrows to match (the vertical
// field of view stays), and the 2D interface keeps the PSP's proportions.
enum class Aspect { Original, Stretch, Fill };
// Fps: the overlay with the frame rate alone, a small number in the corner.
enum class PerfDisplay { Off, Overlay, OverlayAndLog, Log, Fps };
enum class RightStick { Camera, DPad, Off };
// The gamepad button that presses a tilting game's both tilt buttons at once
// (LocoRoco's jump), instead of what it presses otherwise.
enum class JumpButton { Off, LeftStick, South, East, West, North };
// What answers the game when it asks for text such as the player's name.
enum class NameEntry { Keyboard, Fixed };
// Presents per second. The game makes 30 frames a second; the faster rates
// add frames in between with blended movement (frame interpolation), and
// Display follows the display's refresh rate. 30 presents the game's frames
// as they are, as before interpolation existed.
enum class FrameRate { Fps30, Fps45, Fps60, Fps90, Fps120, Display };

// The size the game is drawn at, in pixels. 0x0 follows the window's own
// size in pixels, including every later change of it. 0 x <height> ("1080" in
// settings.ini) is that many lines, the width following the picture's shape
// (video.aspect: the PSP's, the window's, or a fixed shape). A full
// <width>x<height> (older settings) is drawn at that size: under Fill the
// picture has its shape, under Original and Stretch the PSP's shape is fitted
// inside it. The picture is shown with bars where its shape and the window's
// differ.
struct Resolution {
    std::uint32_t width{};
    std::uint32_t height{};
    [[nodiscard]] constexpr bool follows_window() const noexcept { return height == 0u; }
    [[nodiscard]] constexpr bool lines_only() const noexcept { return width == 0u && height != 0u; }
    friend constexpr bool operator==(const Resolution &, const Resolution &) = default;
};
inline constexpr std::uint32_t kMinResolutionSide = 16u;
inline constexpr std::uint32_t kMaxResolutionSide = 8192u;
struct ResolutionPreset {
    const char *name;
    Resolution size;
};
// What the menus offer, in order: lines; the width follows video.aspect. Any
// other size can be set in settings.ini.
inline constexpr ResolutionPreset kResolutionPresets[] = {
    {"Match window", {0u, 0u}},
    {"272 (PSP)", {0u, 272u}},
    {"480", {0u, 480u}},
    {"720", {0u, 720u}},
    {"768", {0u, 768u}},
    {"900", {0u, 900u}},
    {"1080", {0u, 1080u}},
    {"1200", {0u, 1200u}},
    {"1440", {0u, 1440u}},
    {"1600", {0u, 1600u}},
    {"1800", {0u, 1800u}},
    {"2160 (4K)", {0u, 2160u}},
    {"2880 (5K)", {0u, 2880u}},
};
// "window", "<height>" or "<width>x<height>", the spelling settings.ini uses.
[[nodiscard]] std::string format_resolution(Resolution resolution);
[[nodiscard]] bool parse_resolution(const std::string &text, Resolution &out);
// The preset's name, "1920x1080" for a size no preset has.
[[nodiscard]] std::string resolution_name(Resolution resolution);
// video.aspect: "original", "stretch", "fill" (the window's shape) or a shape
// under Fill, "16:9", "16:10", "4:3", "21:9" (43:18, as 3440x1440), "32:9" or
// any "<w>:<h>".
[[nodiscard]] bool parse_aspect(const std::string &text, Aspect &aspect, std::uint32_t &shape_width,
                                std::uint32_t &shape_height);
struct Settings;
[[nodiscard]] std::string format_aspect(const Settings &s);
// Under Fill with a shape: width / height; else 0.
[[nodiscard]] double fill_shape(const Settings &s);

struct Settings {
    // Video
    Resolution resolution{};           // render resolution; 0x0: the window's (see Resolution)
    std::uint32_t window_scale{2u};    // windowed size, multiples of 480x272
    bool fullscreen{};
    bool borderless{};                 // windowed without a frame, covering its display (fullscreen off)
    // The display the window opens on, by its name ("<name> #2" for the second
    // of that name from the left); empty: the primary display.
    std::string monitor;
    PresentMode present_mode{PresentMode::Fifo};
    Aspect aspect{Aspect::Original};
    // Under Fill: the picture's shape, width:height (16:10 is 16 and 10); 0:0
    // the window's own. video.aspect writes it as "fill" or "16:10".
    std::uint32_t shape_width{};
    std::uint32_t shape_height{};
    bool sharp_screen{};               // nearest instead of linear scaling to the window
    bool sharp_textures{};             // nearest instead of linear texture sampling
    std::uint32_t msaa{1u};            // samples per pixel of the game's targets: 1 (off), 2, 4 or 8
    bool texture_pack{true};           // draw an installed HD texture pack's images instead of the game's
    std::string texture_pack_folder;   // a pack used where it is instead of textures/<disc id>; empty: none
    bool unthrottled{};                // let emulated time run ahead of real time
    bool fast_loading{true};           // ...but only while the game loads (kernel/fast_loading.hpp)
    bool disc_in_memory{};             // copy the disc image into memory at start (patch 0038)
    FrameRate frame_rate{FrameRate::Fps30};
    bool frame_rate_auto{true};        // lower the frame rate rather than slow the game
    PerfDisplay perf{PerfDisplay::Off};

    // Text
    std::string font;                  // the game's text font: path, "#face" for a collection; empty: the default
    std::uint32_t font_weight{1u};     // columns the game's glyphs are thickened by, 0 to kMaxFontWeight

    // Audio
    std::uint32_t volume{100u};        // percent
    bool mute{};
    bool soundtrack{true};             // play an imported soundtrack's versions of the music (audio/soundtrack_import_run.hpp)

    // Controls
    // The game's confirm button on the south face button rather than the
    // east one. The default follows the console the profile describes: south
    // where cross confirms, east where circle does.
    bool confirm_south{};
    float dead_zone{0.15f};
    float trigger{0.25f};
    // What LT/RT (L2/R2) press past the trigger point: 0 is L and R like the
    // shoulders, n the game's GameProfile::trigger_profiles[n - 1].
    std::uint32_t trigger_profile{};
    RightStick right_stick{RightStick::Camera};
    float right_stick_zone{0.5f};
    // Drives the ordinary quest camera's yaw and pitch from how far the stick
    // is pushed, instead of the game's fixed-speed turn and vertical presets.
    // On by default. Off writes nothing at all, so the camera is exactly as
    // the game made it.
    bool analog_camera{true};
    // Degrees per second at full deflection, before the stick's own curve.
    float camera_speed{190.0f};
    // Degrees per second at full deflection while a bow or a bowgun aims.
    float aim_speed{90.0f};
    bool invert_camera_x{};
    bool invert_camera_y{};
    // Keyboard and mouse. With the mouse on, the window captures the pointer
    // while the game runs and the mouse turns the camera; Esc frees it.
    bool mouse{true};
    float mouse_sensitivity{0.10f};    // degrees of camera turn per count of mouse motion
    bool invert_mouse_x{};
    bool invert_mouse_y{};
    input::Bindings bindings{input::default_bindings()};
    // On-screen controls for a touch screen, shown once the screen is touched
    // and hidden again when a gamepad or the keyboard is used.
    bool touch_controls{true};
    bool touch_dpad{true};             // the D-pad among them, for the game's menus
    float touch_opacity{0.5f};         // 0.1-1
    float touch_size{1.0f};            // 0.6-1.6 of the default size
    float touch_camera_speed{180.0f};  // degrees the camera turns for a drag across the screen's height
    // Tilt controls, for a game that tilts (GameProfile::tilt): the roll of
    // a gamepad with motion sensors, or of the phone itself, presses the
    // game's tilt buttons (input/tilt.hpp). Off until the player turns it on.
    bool tilt{};
    input::tilt::Mode tilt_mode{input::tilt::Mode::Angle};
    float tilt_full{12.0f};            // degrees of roll from neutral for a full tilt
    float tilt_dead_zone{4.0f};        // degrees of roll from neutral before anything is pressed
    bool tilt_invert{};
    float tilt_level_limit{10.0f};     // Angle + level horizon: degrees the picture turns at most
    // For a game that tilts: the gamepad button that jumps. L3 by default,
    // which no PSP game can read, so it takes nothing from the game.
    JumpButton jump_button{JumpButton::LeftStick};
    NameEntry name_entry{NameEntry::Keyboard};  // on-screen keyboard, or the name below at once
    std::string name{game_default_name()};  // the fixed name

    // Network (ad hoc play through a PSP ad hoc server)
    bool adhoc{};                      // wireless switch on: the game may go on line
    std::string adhoc_server;          // host or host:port of the server; empty: none
    std::string adhoc_nickname;        // shown to other players; empty: the player name
    std::string adhoc_mac;             // this player's virtual MAC, made up on first use
    std::vector<std::string> adhoc_recent;  // sessions joined lately, the latest first
    std::uint32_t adhoc_host_port{27312};   // the built-in server's adhocctl port; the relay is on the next

    // Interface
    bool menu_pause{true};             // opening the menu pauses the game
    bool menu_pause_multiplayer{};     // ...also during ad hoc play, where a paused game stops answering its peers
    bool menu_hint_seen{};             // the "Esc / L3+R3 opens the menu" hint was shown
    std::string last_folder;           // where the setup's file browser was last used

    // Saves
    bool backup_timestamp{true};       // a backup made from the menu goes to a new folder named by its time
};

// How the game's window is shown, kept as the two settings above (an older
// version reads video.fullscreen and sees a borderless window as a window).
// Borderless: a window with no frame over the whole display it is on, so
// switching away and overlays work as with a window.
enum class DisplayMode { Window, Borderless, Fullscreen };
[[nodiscard]] constexpr DisplayMode display_mode(const Settings &s) noexcept {
    return s.fullscreen ? DisplayMode::Fullscreen : s.borderless ? DisplayMode::Borderless : DisplayMode::Window;
}
constexpr void set_display_mode(Settings &s, DisplayMode mode) noexcept {
    s.fullscreen = mode == DisplayMode::Fullscreen;
    s.borderless = mode == DisplayMode::Borderless;
}

inline constexpr std::uint32_t kMaxWindowScale = 4u;
inline constexpr std::uint32_t kMaxFontWeight = 2u;
inline constexpr float kMinMouseSensitivity = 0.01f;
inline constexpr float kMinTouchOpacity = 0.1f;
inline constexpr float kMaxTouchOpacity = 1.0f;
inline constexpr float kMinTouchSize = 0.6f;
inline constexpr float kMaxTouchSize = 1.6f;
inline constexpr float kMinTouchCameraSpeed = 30.0f;
inline constexpr float kMaxTouchCameraSpeed = 720.0f;
inline constexpr float kMaxMouseSensitivity = 0.99f;

// The platforms whose defaults differ. A phone plays full screen with a
// finger or a pad, so a few settings start otherwise there (defaults_for).
enum class Platform { Desktop, Android };
#if defined(__ANDROID__)
inline constexpr Platform kPlatform = Platform::Android;
#else
inline constexpr Platform kPlatform = Platform::Desktop;
#endif
// The defaults on `platform`: Desktop is Settings{} as declared above;
// Android differs in video.aspect (fill: a phone is wider than the PSP),
// video.fullscreen (on: there is no window) and input.mouse (off: a phone
// has no mouse to capture, and an emulator's pointer would turn the camera).
[[nodiscard]] Settings defaults_for(Platform platform);

// Loads the settings on first use, starting from defaults().
[[nodiscard]] Settings &current();
// This platform's defaults, for keys settings.ini lacks and for "Restore
// defaults".
[[nodiscard]] const Settings &defaults();
// Writes current() to settings.ini, leaving values set by environment
// variables at what the file had. Failures are reported on the console.
void save();

// The environment variable that decides the setting stored under `key`
// (for example "video.resolution") for this run, or null.
// The environment variable that decided this setting, or empty when the
// file or the default did. The name carries the game's own prefix.
[[nodiscard]] std::string overridden_by(const char *key);

} // namespace portablekit::settings
