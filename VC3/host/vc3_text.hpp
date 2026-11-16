// The game's text rewritten (vc2-text-overrides.json) and button glyphs drawn
// for the device in use, inside the game's own text: see vc3_text.cpp.
#pragma once

#include "hle/hle_common.hpp"

#include <cstdint>
#include <vector>

namespace portablekit::vc3 {

// Hooks the game's text paths (Europe). VC3_NO_TEXT=1 leaves them alone.
void install_text(Runtime &rt);

// Called by the font's glyph staging (vc3_glyph_begin / vc3_glyph_end in
// vc3_glyphs.cpp): a glyph code of ours is drawn into the staged cell.
void text_glyph_begin(std::uint32_t font, std::uint32_t flags, std::uint32_t code, std::uint32_t table);
void text_glyph_end(std::uint32_t font);

// The HD text upscaler (vc3_text_hd.cpp): one of our glyph cells (16x16 RGBA)
// redrawn at `scale` from its shapes. False for any other cell.
[[nodiscard]] bool text_glyph_cell_hd(const std::uint32_t *cell, std::uint32_t scale, std::vector<std::uint32_t> &out);

}  // namespace portablekit::vc3
