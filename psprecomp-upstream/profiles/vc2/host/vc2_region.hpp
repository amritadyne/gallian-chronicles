// The editions of Valkyria Chronicles II this profile runs, and where the game
// keeps things in each.
//
// Every game address in the profile is written as the European release's
// (ULES01417, the edition research/ENGINE_MAP documents) and passed through
// at() for the edition that is running. An edition's addresses are a table of
// ranges, each shifted by a fixed amount from the European executable, worked
// out by comparing the two executables (research/REGIONS.md).
//
// Before a replacement goes in at a translated function address, code_matches()
// checks the code there against a fingerprint of the European function, so a
// wrong translation leaves the game's own code running instead of patching
// the wrong function.

#pragma once

#include "hle/hle_common.hpp"

#include <cstdint>

namespace portablekit::vc2 {

enum class Region { Europe, Usa, Japan };

// The running edition (from the executable's hash; see vc2_profile.cpp).
[[nodiscard]] Region region();
[[nodiscard]] const char *region_name();

// The running edition's address for a European address. US: the European
// executable's ranges shifted, and .bss, heap, VRAM and hardware addresses as
// they are. Japan: a separate build, so only the addresses listed in its table
// are known; any other address gives 0, so a hook that needs it stays off and
// a diagnostic that reads it finds no memory there.
[[nodiscard]] std::uint32_t at(std::uint32_t eu_address);

// True when the code at at(eu_function) is the European function's, ignoring
// the address fields of its instructions. False for an address that has no
// fingerprint (a function the profile does not replace).
[[nodiscard]] bool code_matches(Runtime &rt, std::uint32_t eu_function);

// The US release's corpus is a library of its own: hands it the profile's
// hooks that cmake/vc2_step_sites.cmake patched into its code (high frame
// rate, camera, aim, LOD, title menu, morale, glyphs). Logs [vc2-region].
// Nothing for the other editions (vc2_corpus_bind.cpp).
void bind_corpus_hooks();

}  // namespace portablekit::vc2
