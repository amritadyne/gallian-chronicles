#pragma once

#include "profile.hpp"

#include <cstdint>

namespace portablekit::vc3 {
// The Action Mode camera from the right stick and the mouse (vc3_camera.cpp).
extern const CameraDriver kCameraDriver;
// The game ran the moved unit's camera code during the previous frame: Action
// Mode (not Command or Target Mode, menus or story). Europe only.
[[nodiscard]] bool action_mode();
// The Target Mode aim code ran during the previous frame (Modern; Europe).
[[nodiscard]] bool target_mode();
// A screen cursor (the academy map's) moved by the mouse during the previous
// frame.
[[nodiscard]] bool cursor_mode();
// The academy map's camera taken with the right mouse button (the game's
// Triangle held): while held, the mouse drives the stick (x right, y down,
// -1..1 this frame) instead of the cursor.
void set_map_camera_hold(bool held);
[[nodiscard]] bool map_camera_stick(float &x, float &y);
}  // namespace portablekit::vc3

// Called by the patched turning code (cmake/vc3_step_sites.cmake) once the
// game's own turn of `unit`'s camera is done.
extern "C" void vc3_camera_turn(std::uint32_t unit) noexcept;
// Called before the game reads its camera buttons (Modern controls note the
// unit's angles there, to undo the game's own turn).
extern "C" void vc3_camera_begin(std::uint32_t unit) noexcept;
// Called by the screen cursor controller (the academy map's) right after it
// reads its stick (vc3_camera.cpp): the mouse moves the cursor.
extern "C" void vc3_aim_stick(std::uint32_t sp, std::uint32_t obj) noexcept;
// Called by the Target Mode aim code before it scales its nub by the step
// (vc3_camera.cpp): the right stick and mouse aim.
extern "C" void vc3_aim_target(float *x, float *y, float step) noexcept;
