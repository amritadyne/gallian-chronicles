// VC2 Action Mode camera from the right stick and the mouse.
//
// The game turns the camera of the unit being moved in 0x0888FC94(unit): L
// and R (its own pad bits 0x400 / 0x800) add a fixed rate to the yaw at
// unit+1432, and a virtual stick made of two face buttons moves the pitch at
// unit+1428, only while unit+876 is 1 or 2. Both are 16-bit angles stored in
// a word: 65536 for a full turn, so 182.044 a degree. The invert switches of
// the game's options sit at EU 0x08D5DA08 (+0 vertical, +4 horizontal); 0
// means the game negates its own turn. The pitch is clamped later by the game
// (0x088906B4), so it is left alone here.
//
// cmake/vc2_step_sites.cmake makes both ends of the turning code (EU
// L_0888FECC on the usual path, L_08890028 on the other) call
// vc2_camera_turn(unit) once the game's own turn is done. What the stick and
// the mouse asked for since the previous frame (PortableKit's camera input:
// Camera speed, Mouse sensitivity and their invert settings) is added there,
// left and right the same way round as the game's own turn with the game's
// invert options; up and down the other way (pushing up looks up), which the
// owner preferred on a stick. PortableKit's invert settings apply on top.
//   VC2_CAMERA=0           off: the right stick and the mouse do nothing
//   VC2_CAMERA_FLIP_X=1    turns the other way left and right
//   VC2_CAMERA_FLIP_Y=1    turns the other way up and down
// Modern controls (vc2_controls.cpp, the default) also cancel the game's own
// turning from the buttons (L / R, and Triangle / Cross for the pitch): the
// begin sites (EU L_0888FD8C and L_0888FF00, before the game reads its buttons
// on each path) note the unit's yaw and pitch, and vc2_camera_turn puts them
// back before adding the stick and mouse. The game's L + R reset comes after
// and still works (Modern sends it for R3). Classic controls turn the right
// stick and mouse camera off and leave the game's own turning alone.
// action_mode() tells the controls whether the game ran this camera code
// during the previous frame: Action Mode, as opposed to Command Mode, Target
// Mode (unit+876 == 3 skips it), menus and story.
// Every release (Europe, the US and Japan), like the sites.
#include "vc2_camera.hpp"
#include "vc2_controls.hpp"
#include "vc2_glyphs.hpp"
#include "vc2_region.hpp"
#include "vc2_tick.hpp"

#include "camera/camera_input.hpp"
#include "kernel/kernel.hpp"
#include "profile.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

namespace portablekit::vc2 {
namespace {

constexpr float kUnitsPerDegree = 65536.0f / 360.0f;
constexpr std::uint32_t kEuInvertOptions = 0x08D5DA08u;

struct CameraState {
    camera::Turn pending{};   // what the next turn applies
    std::atomic<bool> turned{false};   // the game ran its turning code since the last flip
    std::atomic<bool> driving{false};  // it did during the frame before (Action Mode)
    std::uint64_t turns = 0u, frames = 0u, mode_changes = 0u;
    // The unit's angles before the game's own button turning (Modern).
    std::uint32_t begin_unit = 0u, begin_yaw = 0u, begin_pitch = 0u;
    // Target Mode aim (Modern): the aim code ran since the last flip, and
    // during the frame before; the mouse's degrees for the next aim.
    std::atomic<bool> aimed{false}, aiming{false};
    camera::Turn aim_mouse{};
    std::uint64_t aims = 0u;
    // A screen cursor (the academy map's): its controller ran since the last
    // flip, and during the frame before; the mouse's degrees for it.
    std::atomic<bool> cursor_ran{false}, cursor{false};
    // The academy map's camera with the right mouse button held (the game's
    // Triangle, vc2_controls.cpp): the mouse as the stick that turns it.
    std::atomic<bool> map_hold{false};
    std::atomic<float> map_x{0.0f}, map_y{0.0f};
    camera::Turn cursor_mouse{};
    std::uint64_t cursor_moves = 0u;
};

CameraState &state() {
    static CameraState s;
    return s;
}

bool flag(const char *name) {
    const char *value = env(name);
    return value != nullptr && *value != '\0' && std::string(value) != "0";
}

// The sites are patched into every edition's corpus (cmake/vc2_step_sites.cmake).
bool europe() { return true; }

// The right stick and mouse camera: Modern controls, unless VC2_CAMERA=0.
bool enabled() {
    static const bool on = [] {
        const char *value = env("CAMERA");
        return europe() && controls_preset() == ControlsPreset::Modern &&
               !(value != nullptr && std::string(value) == "0");
    }();
    return on;
}

bool modern() {
    static const bool on = europe() && controls_preset() == ControlsPreset::Modern;
    return on;
}

void frame(psprecomp::Runtime &) {
    glyph_frame();
    CameraState &s = state();
    ++s.frames;
    const bool driving = s.turned.exchange(false);
    const bool aiming = s.aimed.exchange(false);
    const bool cursor = s.cursor_ran.exchange(false);
    s.cursor = cursor;
    if (driving != s.driving.exchange(driving) && ++s.mode_changes <= 60u)
        std::cerr << "[vc2-camera-input] " << (driving ? "Action Mode" : "left Action Mode") << " at flip " << s.frames
                  << "\n";
    if (aiming != s.aiming.exchange(aiming) && ++s.mode_changes <= 60u)
        std::cerr << "[vc2-camera-input] " << (aiming ? "Target Mode" : "left Target Mode") << " at flip " << s.frames
                  << "\n";
    s.pending = {};
    s.aim_mouse = {};
    s.cursor_mouse = {};
    s.map_x = 0.0f;
    s.map_y = 0.0f;
    if (enabled() && s.map_hold.load()) {
        // The mouse's degrees this frame against what a stick at full tilt
        // turns in one (Camera speed x the frame's length), as for the aim.
        const portablekit::camera::Turn mouse = camera::take(camera::Source::Mouse);
        const float step = corrected_step() > 0.0f ? corrected_step() : 1.0f;
        const float full = std::max(1.0f, portablekit::settings::current().camera_speed) * step / 59.94f;
        s.map_x = std::clamp(mouse.yaw_degrees / full, -1.0f, 1.0f);
        s.map_y = std::clamp(mouse.pitch_degrees / full, -1.0f, 1.0f);
        camera::discard();
        return;
    }
    if (enabled()) {
        if (driving) {
            s.pending = camera::take();
            return;
        }
        // Target Mode: the mouse's degrees; the sticks are read as held rates.
        if (aiming) s.aim_mouse = camera::take(camera::Source::Mouse);
        // A screen cursor: the mouse only (the right stick never moves it).
        else if (cursor) s.cursor_mouse = camera::take(camera::Source::Mouse);
    }
    // Nothing else may build up for later.
    camera::discard();
}

bool driving() {
    const CameraState &s = state();
    // The academy map's camera taken with the right button counts too: without
    // it PortableKit did not hand the mouse's motion over (2026-11-09).
    return enabled() && (s.driving.load() || s.aiming.load() || s.cursor.load() || s.map_hold.load());
}

std::int32_t units(float degrees) { return static_cast<std::int32_t>(std::lround(degrees * kUnitsPerDegree)); }

}  // namespace

const CameraDriver kCameraDriver{
    .frame = &frame,
    .driving = &driving,
};

bool action_mode() { return europe() && state().driving.load(); }
bool target_mode() { return europe() && state().aiming.load(); }
bool cursor_mode() { return europe() && state().cursor.load(); }
void set_map_camera_hold(bool held) { state().map_hold = held; }
bool map_camera_stick(float &x, float &y) {
    const CameraState &s = state();
    if (!s.map_hold.load()) return false;
    x = s.map_x.load();
    y = s.map_y.load();
    return true;
}

}  // namespace portablekit::vc2

extern "C" void vc2_camera_begin(std::uint32_t unit) noexcept {
    using namespace portablekit::vc2;
    if (!modern() || unit == 0u) return;
    CameraState &s = state();
    auto &memory = portablekit::kernel().runtime().memory();
    s.begin_unit = unit;
    s.begin_yaw = memory.load32(unit + 1432u);
    s.begin_pitch = memory.load32(unit + 1428u);
}

extern "C" void vc2_camera_turn(std::uint32_t unit) noexcept {
    using namespace portablekit::vc2;
    if (!europe() || unit == 0u) return;
    CameraState &s = state();
    s.turned = true;
    auto &memory = portablekit::kernel().runtime().memory();
    if (s.begin_unit == unit) {
        // Modern: undo the game's own turn from the buttons.
        const std::uint32_t yaw = memory.load32(unit + 1432u), pitch = memory.load32(unit + 1428u);
        static std::uint32_t undone = 0u;
        if ((yaw != s.begin_yaw || pitch != s.begin_pitch) && ++undone <= 40u)
            std::cerr << "[vc2-camera-input] undid the game's own turn of unit 0x" << std::hex << unit << std::dec
                      << ": yaw " << static_cast<std::int32_t>(yaw - s.begin_yaw) << ", pitch "
                      << static_cast<std::int32_t>(pitch - s.begin_pitch) << " (" << undone << ")\n";
        memory.store32(unit + 1432u, s.begin_yaw);
        memory.store32(unit + 1428u, s.begin_pitch);
    } else if (modern()) {
        static std::uint32_t missed = 0u;
        if (++missed <= 10u)
            std::cerr << "[vc2-camera-input] turn of unit 0x" << std::hex << unit << " without a begin (begin unit 0x"
                      << s.begin_unit << std::dec << ")\n";
    }
    s.begin_unit = 0u;
    if (!enabled()) return;
    const portablekit::camera::Turn turn = s.pending;
    s.pending = {};
    if (turn.yaw_degrees == 0.0f && turn.pitch_degrees == 0.0f) return;
    static const bool flip_x = flag("CAMERA_FLIP_X"), flip_y = flag("CAMERA_FLIP_Y");
    if (turn.yaw_degrees != 0.0f) {
        std::int32_t delta = units(turn.yaw_degrees);
        if (memory.load32(at(kEuInvertOptions) + 4u) == 0u) delta = -delta;
        if (flip_x) delta = -delta;
        memory.store32(unit + 1432u, memory.load32(unit + 1432u) + static_cast<std::uint32_t>(delta));
    }
    const std::uint32_t mode = memory.load32(unit + 876u);
    if (turn.pitch_degrees != 0.0f && (mode == 1u || mode == 2u)) {
        std::int32_t delta = units(turn.pitch_degrees);
        // Pushing up looks up by default (owner, 2026-11-08: the game's own
        // way round felt inverted on a stick).
        if (memory.load32(at(kEuInvertOptions)) == 0u) delta = -delta;
        if (flip_y) delta = -delta;
        memory.store32(unit + 1428u, memory.load32(unit + 1428u) + static_cast<std::uint32_t>(delta));
    }
    if (++s.turns % 600u == 1u)
        std::cerr << "[vc2-camera-input] turning unit 0x" << std::hex << unit << std::dec << " (state " << mode
                  << "), " << s.turns << " turns\n";
}

// A screen cursor and the mouse (Modern). The cursor controller 0x08B09910(obj)
// (the academy map's cursor; earlier taken for the Target Mode reticle) reads
// its virtual stick (0x08835714 into a vector on its stack) and, unless the
// stick is inside 0.001, moves the cursor (obj+176 x, obj+180 y, PSP pixels)
// by the stick x speed x the step; then 0x08B09730 clamps it to
// obj+192..+204, places the sprite and tells its owner. cmake makes it call
// vc2_aim_stick(sp, obj) right after the stick read (EU L_08B09990). The
// mouse moves the cursor directly, VC2_CURSOR_SCALE PSP pixels a degree of
// mouse turn (default 1.6); if the game's own stick is idle, a push of 0.002
// makes the game run its clamp and placement for the new position. The right
// stick does not move it (owner, 2026-11-08). First step of full mouse control.
//   VC2_CURSOR_FLIP_X=1 / VC2_CURSOR_FLIP_Y=1   the other way round
extern "C" void vc2_aim_stick(std::uint32_t sp, std::uint32_t obj) noexcept {
    using namespace portablekit::vc2;
    if (!enabled() || obj == 0u) return;
    CameraState &s = state();
    s.cursor_ran = true;
    const portablekit::camera::Turn turn = s.cursor_mouse;
    s.cursor_mouse = {};
    if (turn.yaw_degrees == 0.0f && turn.pitch_degrees == 0.0f) return;
    static const float scale = [] {
        if (const char *text = portablekit::env("CURSOR_SCALE")) {
            const float value = std::strtof(text, nullptr);
            if (value > 0.01f && value < 100.0f) return value;
        }
        return 1.6f;
    }();
    static const bool flip_x = flag("CURSOR_FLIP_X"), flip_y = flag("CURSOR_FLIP_Y");
    auto &memory = portablekit::kernel().runtime().memory();
    // PortableKit: positive yaw is the mouse moved right, positive pitch the
    // mouse moved down (towards the player): right and down the screen.
    const float dx = turn.yaw_degrees * scale * (flip_x ? -1.0f : 1.0f);
    const float dy = turn.pitch_degrees * scale * (flip_y ? -1.0f : 1.0f);
    const auto add = [&](std::uint32_t address, float delta) {
        memory.store32(address, std::bit_cast<std::uint32_t>(std::bit_cast<float>(memory.load32(address)) + delta));
    };
    add(obj + 176u, dx);
    add(obj + 180u, dy);
    const float stick_x = std::bit_cast<float>(memory.load32(sp)), stick_y = std::bit_cast<float>(memory.load32(sp + 4u));
    if (std::fabs(stick_x) + std::fabs(stick_y) < 0.002f) memory.store32(sp, std::bit_cast<std::uint32_t>(0.002f));
    if (++s.cursor_moves % 600u == 1u)
        std::cerr << "[vc2-camera-input] mouse moves cursor 0x" << std::hex << obj << std::dec << " by " << dx << ", "
                  << dy << " (now " << std::bit_cast<float>(memory.load32(obj + 176u)) << ", "
                  << std::bit_cast<float>(memory.load32(obj + 180u)) << "), " << s.cursor_moves << " moves\n";
}

// Target Mode aim (Modern). The aim code (EU 0x088E1C.., reached in Target
// Mode) reads virtual stick 1 (the nub) through 0x08835714, drops each axis
// inside 0.2, multiplies both by the step (time block +28) at 0x088E1D60 and
// hands them to 0x088B2CA0(aim, 0): yaw (aim+228, 65536 a turn) -= x x gain
// (EU 0x091E8AE8) x 182.04, pitch (aim+244) += y x gain (EU 0x091E8B0C),
// clamped (aim+236..+240). cmake makes 0x088E1D60 call vc2_aim_target(&x, &y,
// step) first. The right stick (and the camera keys) are added as a second
// nub, the same rate as the left stick at full tilt; the mouse's degrees are
// turned into a nub value: its degrees this frame over what the right stick
// at full tilt turns in a frame (Camera speed x step / 59.94 s), so the mouse
// and the right stick agree. Left and right follow
// PortableKit's sign (right positive); up and down are the other way round
// from its pitch, so pushing up aims up.
//   VC2_AIM_FLIP_X=1 / VC2_AIM_FLIP_Y=1   the other way round
extern "C" void vc2_aim_target(float *x, float *y, float step) noexcept {
    using namespace portablekit::vc2;
    if (!enabled() || x == nullptr || y == nullptr) return;
    CameraState &s = state();
    s.aimed = true;
    const portablekit::camera::Turn mouse = s.aim_mouse;
    s.aim_mouse = {};
    const portablekit::camera::Rate stick = portablekit::camera::rate(portablekit::camera::Source::Stick);
    const portablekit::camera::Rate keys = portablekit::camera::rate(portablekit::camera::Source::Keys);
    float add_x = stick.yaw + keys.yaw, add_y = stick.pitch + keys.pitch;
    if ((mouse.yaw_degrees != 0.0f || mouse.pitch_degrees != 0.0f) && step > 1e-6f) {
        // The mouse as a nub: its degrees this frame against what the right
        // stick at full tilt gives in a frame (Camera speed x the frame's
        // length, step / 59.94 s). (Dividing by the game's own aim gains, EU
        // 0x091E8AE8 / 0x091E8B0C, did nothing: they read 0 there, 19:53.)
        const float speed = std::max(1.0f, portablekit::settings::current().camera_speed);
        const float full_tilt_degrees = speed * step / 59.94f;
        add_x += mouse.yaw_degrees / full_tilt_degrees;
        add_y += mouse.pitch_degrees / full_tilt_degrees;
        static std::uint32_t logged = 0u;
        if (++logged <= 3u)
            std::cerr << "[vc2-camera-input] Target Mode mouse aim: " << mouse.yaw_degrees << ", " << mouse.pitch_degrees
                      << " degrees = nub " << mouse.yaw_degrees / full_tilt_degrees << ", "
                      << mouse.pitch_degrees / full_tilt_degrees << " (full tilt " << full_tilt_degrees
                      << " degrees a frame)\n";
    }
    if (add_x == 0.0f && add_y == 0.0f) return;
    static const bool flip_x = flag("AIM_FLIP_X"), flip_y = flag("AIM_FLIP_Y");
    *x += flip_x ? -add_x : add_x;
    // The nub's y runs the other way from PortableKit's pitch: pushing the
    // right stick up must aim up (owner, 2026-11-08 19:50: it was inverted).
    *y += flip_y ? add_y : -add_y;
    if (++s.aims % 600u == 1u)
        std::cerr << "[vc2-camera-input] Target Mode aim: added " << add_x << ", " << add_y << " to the nub ("
                  << s.aims << " aims)\n";
}

// Fine aim from the stick's inner zone (Modern; owner, 2026-11-09: "make the
// current deadzone behave like the D-pad, but 3D"). The aim code reads the
// nub (virtual stick 1) and drops each axis inside 0.2; only when both are 0
// (and the right stick and mouse added nothing) does it read the D-pad
// (virtual stick 2, at 0x088E1DB4) and aim with it in its precise mode
// (0x088B2CA0(aim, x, y, 1): slow). cmake makes the aim code call
// vc2_aim_nub(sp) once it has read the nub (EU L_088E1CEC, unit 0055: the
// nub as read, before that 0.2) and vc2_aim_fine(sp) once it has read the
// D-pad (L_088E1DBC). When the D-pad is up and the nub was inside the 0.2 but
// past a little noise, the D-pad is given the nub's direction, scaled so the
// edge of the inner zone is a full D-pad press: precise aiming in any
// direction, as gentle as the push.
//   VC2_NO_FINE_AIM=1   leaves the inner zone dead
namespace {
struct FineAim {
    float x = 0.0f, y = 0.0f;  // the nub as the aim code read it
    std::uint64_t uses = 0u;
};
FineAim &fine_aim() {
    static FineAim f;
    return f;
}
constexpr float kAimDeadZone = 0.2f;   // the aim code's own, each axis (EU 0x088E1CF4)
constexpr float kFineNoise = 0.02f;    // less than this is left alone
}  // namespace

extern "C" void vc2_aim_nub(std::uint32_t sp) noexcept {
    using namespace portablekit::vc2;
    if (!modern() || sp == 0u) return;
    auto &memory = portablekit::kernel().runtime().memory();
    fine_aim().x = std::bit_cast<float>(memory.load32(sp));
    fine_aim().y = std::bit_cast<float>(memory.load32(sp + 4u));
}

extern "C" void vc2_aim_fine(std::uint32_t sp) noexcept {
    using namespace portablekit::vc2;
    static const bool off = flag("NO_FINE_AIM");
    if (off || !modern() || sp == 0u) return;
    FineAim &f = fine_aim();
    if (std::fabs(f.x) >= kAimDeadZone || std::fabs(f.y) >= kAimDeadZone) return;
    const float length = std::sqrt(f.x * f.x + f.y * f.y);
    if (length < kFineNoise) return;
    auto &memory = portablekit::kernel().runtime().memory();
    const float pad_x = std::bit_cast<float>(memory.load32(sp + 16u));
    const float pad_y = std::bit_cast<float>(memory.load32(sp + 20u));
    if (pad_x != 0.0f || pad_y != 0.0f) return;  // the real D-pad wins
    // The inner zone's edge (0.2) is a full press; never past 1.
    const float scale = std::min(1.0f / kAimDeadZone, 1.0f / length);
    memory.store32(sp + 16u, std::bit_cast<std::uint32_t>(f.x * scale));
    memory.store32(sp + 20u, std::bit_cast<std::uint32_t>(f.y * scale));
    if (++f.uses <= 5u || f.uses % 2000u == 0u)
        std::cerr << "[vc2-camera-input] fine aim from the stick's inner zone: nub " << f.x << ", " << f.y
                  << " -> D-pad " << f.x * scale << ", " << f.y * scale << " (" << f.uses << " frames)\n";
}

// Movement trace (2026-11-09, to measure the stick's dead zone for moving a
// unit). Action Mode's movement reads the nub through 0x08835714 in
// 0x088D37C0 (EU) and turns it by the camera; cmake makes it call
// vc2_move_stick(vector) once read (L_088D388C, unit 0051). In Action Mode,
// each change of the vector by 0.05 or more is logged with the pad's analog
// bytes the game was last given, up to 400 lines.
extern "C" void vc2_move_stick(std::uint32_t vector, std::uint32_t sp) noexcept {
    using namespace portablekit::vc2;
    if (!europe() || vector == 0u || !action_mode()) return;
    static std::uint32_t logged = 0u;
    static float last_x = 0.0f, last_y = 0.0f;
    if (logged >= 400u) return;
    auto &memory = portablekit::kernel().runtime().memory();
    const float x = std::bit_cast<float>(memory.load32(vector));
    const float y = std::bit_cast<float>(memory.load32(vector + 4u));
    int peek_x = 0, peek_y = 0, peek_dpad = 0;
    last_analog(peek_x, peek_y, peek_dpad);
    // The unit: the caller's s0, saved at sp+28 by 0x088D37C0; its state at
    // +444 (0x2715 and 0x2712 seen in the caller 0x088FBF60).
    std::uint32_t unit_state = 0u;
    if (const std::uint32_t unit = memory.load32(sp + 28u); unit >= 0x08800000u && unit < 0x0A000000u)
        unit_state = memory.load32(unit + 444u);
    static int last_dpad = 0;
    static std::uint32_t last_state = 0u;
    if (std::fabs(x - last_x) < 0.05f && std::fabs(y - last_y) < 0.05f && peek_dpad == last_dpad &&
        unit_state == last_state)
        return;
    last_x = x, last_y = y, last_dpad = peek_dpad, last_state = unit_state;
    ++logged;
    int pad_x = 0, pad_y = 0, dpad = 0;
    last_analog(pad_x, pad_y, dpad);
    std::cerr << "[vc2-move] stick vector " << x << ", " << y << " (length " << std::sqrt(x * x + y * y)
              << ") from pad analog " << pad_x << ", " << pad_y << ", D-pad 0x" << std::hex << dpad
              << ", unit state 0x" << unit_state << std::dec << "\n";
}

// Camera moves in R&D's vehicle preview (and other users of the same mover,
// EU 0x08A77E44 sets up, 0x08A7869C runs each frame): the eye and target move
// by a fixed velocity a frame, set to distance x (frame interval / N), and the
// move ends only when the eye lands within 0.001 of its destination. At the
// game's rate the interval makes N a whole number of frames; at a high frame
// rate (interval 59.94 / rate) it does not, the eye steps past the
// destination and the camera drifts away for ever (the tank shrank to a dot
// once a part list opened). The arrival test (L_08A78944, unit 0157) calls
// vc2_preview_arrived(arrived, distance, mover): the move also ends at its
// closest approach, within half a frame's step.
extern "C" std::uint32_t vc2_preview_arrived(std::uint32_t arrived, float distance, std::uint32_t mover) noexcept {
    if (arrived != 0u || mover < 0x08800000u || mover >= 0x0A000000u) return arrived;
    auto &memory = portablekit::kernel().runtime().memory();
    const float vx = std::bit_cast<float>(memory.load32(mover + 0x60u));
    const float vy = std::bit_cast<float>(memory.load32(mover + 0x64u));
    const float vz = std::bit_cast<float>(memory.load32(mover + 0x68u));
    const float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (!(speed > 0.0f) || std::fabs(distance) > speed * 0.5f + 0.001f) return arrived;
    static std::uint32_t logged = 0u;
    if (++logged <= 5u)
        std::cerr << "[vc2-preview] camera move ended at its closest approach (" << std::fabs(distance)
                  << " from the destination, step " << speed << ")\n";
    return 1u;
}
