// VC2 controls presets: Modern (VC4) and Classic (PSP). See vc3_controls.cpp.
#pragma once

#include "hle/hle_common.hpp"

namespace portablekit::vc3 {

enum class ControlsPreset { Modern, Classic };

// VC3_CONTROLS=modern (default) or classic. Modern needs the Europe release
// (its camera sites); elsewhere it is Classic.
[[nodiscard]] ControlsPreset controls_preset();

// Wraps the game's only pad read (sceCtrlPeekBufferPositive, and ReadBuffer
// for safety) to apply the Modern preset's per-mode remap. Register it before
// anything else that wraps those functions (the census), so they see what the
// game sees.
void register_controls(HleRegistrar &hle);

// The context of the last pad read (Modern): 0 Action Mode, 1 Target Mode,
// 2 the academy map, 3 anywhere else.
[[nodiscard]] int controls_context();

// The analog bytes and D-pad bits (0x10 up, 0x20 right, 0x40 down, 0x80
// left) the game was last given (diagnostics).
void last_analog(int &x, int &y, int &dpad);

// What gives a PSP button on the keyboard and mouse in that context (Modern):
// a mouse button (1 left, 3 right) or a key cap's label. False when nothing
// gives exactly that button there (or the preset is Classic).
struct KbmPrompt {
    int mouse = 0;
    const char *cap = nullptr;
};
[[nodiscard]] bool kbm_prompt(std::uint32_t psp_button, KbmPrompt &out);
// The same in a given context (0 Action Mode, 1 Target Mode, 2 academy map,
// 3 elsewhere): the tutorials' hint labels are about Action Mode, whatever is
// open over it while they show.
[[nodiscard]] bool kbm_prompt_in(std::uint32_t psp_button, int context, KbmPrompt &out);

}  // namespace portablekit::vc3
