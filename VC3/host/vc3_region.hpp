// Valkyria Chronicles III: which edition is running, and where the game keeps
// things.
//
// This profile is VC2's (profiles/vc2/host, renamed vc3_*). Its code still
// writes every guest address as Valkyria Chronicles II Europe's (ULES01417):
// that is what its comments, research notes and measurements refer to. at()
// gives VC3's address for one, from a table made by aligning the two
// executables (analysis/claude/gen_table.py, map_vc3.py): only addresses whose
// code is the same in both games (12-word windows, address fields masked, that
// occur once in each; data from the relocation targets of that code) are in
// it. Any other address gives 0, so a hook that needs it stays off.
//
// Addresses measured in VC3 itself (its step sites, LOD and camera sites in
// cmake/vc3_sites.cmake and the tables of vc3_tick.cpp) are VC3's own and do
// not go through at().
//
// The Japanese Extra Edition (ULJM-05957) and the English ReTranslation v1.0.8
// have the same code byte for byte (only text differs), so every address is
// the same in both.

#pragma once

#include "hle/hle_common.hpp"

#include <cstdint>

namespace portablekit::vc3 {

// VC2's releases, kept so VC2's code compiles unchanged: VC3 is never one of
// them, so VC2-only paths (`region() == Region::Europe`) stay off.
enum class Region { Europe, Usa, Japan, Vc3 };
enum class Edition { Japanese, English };

[[nodiscard]] Region region();
[[nodiscard]] Edition edition();
[[nodiscard]] const char *region_name();

// VC2's own interface additions (title menu SETTINGS, the in-game Settings
// screen drawn from VC2's Options screen, the "Settings" row under VC2's
// menus' Options, tab key badges): drawn from VC2's own screens, so off in
// VC3 until measured there.
inline constexpr bool kVc2Interface = false;

// VC3's address for a VC2 Europe address; 0 when it is not known.
[[nodiscard]] std::uint32_t at(std::uint32_t eu_address);

// True when the code at at(eu_function) is VC2's function, ignoring the address
// fields of its instructions (all 67 were checked against VC3 when the table
// was made). False for an address that has no fingerprint.
[[nodiscard]] bool code_matches(Runtime &rt, std::uint32_t eu_function);

}  // namespace portablekit::vc3
