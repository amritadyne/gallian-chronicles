// The profile's hooks in the generated code of another edition (the US
// release's corpus, a library of its own: corpora/us.dll).
//
// cmake/vc2_step_sites.cmake patches calls such as vc2_step_trunc() into the
// generated code. In the European corpus, linked into the executable, they
// reach the profile's functions directly. An edition's corpus library needs
// nothing from the executable (psprecomp/corpus_abi.hpp), so there the same
// names are small forwarders (host/vc2_corpus_shim.cpp, built into that
// library) that call through this table, which the profile hands the library
// once its image is loaded (vc2::bind_corpus_hooks, vc2_region.cpp). Until it
// is handed over, the forwarders do what the original instructions did.
//
// The sites pass the European address of each site (`pc`), so the profile's
// per-site tables serve every edition.

#pragma once

#include <cstdint>

struct Vc2CorpusHooks {
    std::uint32_t version;  // kVc2CorpusHooksVersion
    std::uint32_t (*step_trunc)(std::uint32_t pc, float value) noexcept;
    float (*frame_scale)(std::uint32_t pc, float value) noexcept;
    std::uint32_t (*frame_count)(std::uint32_t pc, std::uint32_t amount) noexcept;
    float (*lod_distance)(std::uint32_t address, float value) noexcept;
    void (*lod_params_begin)(std::uint32_t destination) noexcept;
    void (*lod_params_end)() noexcept;
    void (*camera_turn)(std::uint32_t unit) noexcept;
    void (*camera_begin)(std::uint32_t unit) noexcept;
    void (*aim_stick)(std::uint32_t sp, std::uint32_t obj) noexcept;
    void (*aim_target)(float *x, float *y, float step) noexcept;
    std::uint32_t (*preview_arrived)(std::uint32_t arrived, float distance, std::uint32_t mover) noexcept;
    std::uint32_t (*title_list_select)(std::uint32_t list, std::uint32_t index) noexcept;
    std::uint32_t (*title_choice)(std::uint32_t index) noexcept;
    void (*aim_nub)(std::uint32_t sp) noexcept;
    void (*aim_fine)(std::uint32_t sp) noexcept;
    void (*move_stick)(std::uint32_t vector, std::uint32_t sp) noexcept;
    std::uint32_t (*morale_step)(std::uint32_t amount, std::uint32_t position, std::uint32_t morale) noexcept;
    void (*glyph_begin)(std::uint32_t font, std::uint32_t flags, std::uint32_t code, std::uint32_t table) noexcept;
    void (*glyph_end)(std::uint32_t font) noexcept;
};

inline constexpr std::uint32_t kVc2CorpusHooksVersion = 1u;

// The corpus library's entry point that takes the table (returns the
// library's own version, so a mismatch is reported).
inline constexpr const char *kVc2CorpusBindSymbol = "vc2_corpus_bind";
using Vc2CorpusBind = std::uint32_t (*)(const Vc2CorpusHooks *hooks);
