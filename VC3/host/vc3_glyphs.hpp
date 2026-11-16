#pragma once

#include <string>

namespace portablekit::vc3 {
// Draws the button prompts of the device in use (vc3_glyphs.cpp): hands the
// renderer its texture override (PortableKit patch 0020). VC3_NO_PROMPTS=1
// leaves the game's own.
void install_prompts();
// Once a flip (the camera driver's frame): follows the device in use, and the
// glyph sheet inventory while VC3_GLYPH_TRACE is set.
void glyph_frame();
// The prompt style setting changed (the in-game Settings screen): the sheet
// is painted again for it.
void prompts_changed();
// The prompts are the keyboard's (Modern preset): the tab badges show keys.
[[nodiscard]] bool keyboard_prompts();
// The glyphs in use: 0 PlayStation, 1 Xbox, 2 keyboard and mouse (Modern
// controls; with Classic, or the prompts off, PlayStation).
[[nodiscard]] int prompt_device();
// VC3_TUTORIAL_CAPTURE=<folder>: the text a paged box (tutorials) was given,
// noted in the capture's index beside the textures made after it.
void capture_note_text(const std::string &cp932);
}  // namespace portablekit::vc3
