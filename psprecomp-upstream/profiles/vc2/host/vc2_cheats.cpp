// Cheats for the texture-dump runs only (2026-11-12, owner's request: running
// around every map quickly). Off unless set; "Play VC2 (texture dump).cmd"
// sets them, nothing else does.
//
//   VC2_CHEAT_GODMODE=1   battle units' HP held at 9999
//   VC2_CHEAT_AP=1        battle units' AP held at 1000
//
// The battle unit table is the one the public CWCheat codes for ULES01417 use
// ("Infinite HP (All Chars)" / "Infinite AP (All Chars)"): 33 entries of 0x100
// bytes from 0x0907BD30, +0 HP (int), +4 AP (float). Those codes write every
// entry all the time; here an entry is written only when it holds a living
// unit (HP 1..9999 and AP a sane float) and at least two entries do (a battle
// is on), so the table's memory is left alone outside battles and fallen
// units stay down. Like the codes, it does not tell sides apart: enemies keep
// their HP too. Written at every vblank (60 a second), so a unit can't run
// dry between two writes at any frame rate. Europe only (the US table is
// elsewhere).

#include "vc2_cheats.hpp"
#include "vc2_region.hpp"

#include "profile.hpp"
#include "kernel/kernel.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace portablekit::vc2 {
namespace {

constexpr std::uint32_t kEuUnitTable = 0x0907BD30u;
constexpr std::uint32_t kUnitStride = 0x100u;
constexpr int kUnits = 33;
constexpr std::uint32_t kHpOffset = 0u, kApOffset = 4u;
constexpr std::uint32_t kGodHp = 9999u;
constexpr float kFullAp = 1000.0f;

struct CheatState {
    bool godmode = false, ap = false;
    bool in_battle = false;
    std::uint64_t battles = 0u;
};

CheatState &state() {
    static CheatState s;
    return s;
}

bool on(const char *name) {
    const char *v = env(name);
    return v != nullptr && *v != '\0' && std::strcmp(v, "0") != 0;
}

bool living(std::uint32_t hp, float ap) {
    return hp >= 1u && hp <= kGodHp && std::isfinite(ap) && ap >= 0.0f && ap <= 100000.0f;
}

void on_vblank() {
    CheatState &s = state();
    auto &memory = kernel().runtime().memory();
    int alive = 0;
    for (int i = 0; i < kUnits; ++i) {
        const std::uint32_t unit = kEuUnitTable + static_cast<std::uint32_t>(i) * kUnitStride;
        if (living(memory.load32(unit + kHpOffset), std::bit_cast<float>(memory.load32(unit + kApOffset)))) ++alive;
    }
    const bool battle = alive >= 2;
    if (battle != s.in_battle) {
        s.in_battle = battle;
        if (battle) ++s.battles;
        std::cerr << "[vc2-cheats] " << (battle ? "battle units found (" + std::to_string(alive) + "): cheats on"
                                                 : std::string("no battle units: cheats idle"))
                  << "\n";
    }
    if (!battle) return;
    for (int i = 0; i < kUnits; ++i) {
        const std::uint32_t unit = kEuUnitTable + static_cast<std::uint32_t>(i) * kUnitStride;
        const std::uint32_t hp = memory.load32(unit + kHpOffset);
        const float ap = std::bit_cast<float>(memory.load32(unit + kApOffset));
        if (!living(hp, ap)) continue;
        if (s.godmode && hp != kGodHp) memory.store32(unit + kHpOffset, kGodHp);
        if (s.ap && ap < kFullAp) memory.store32(unit + kApOffset, std::bit_cast<std::uint32_t>(kFullAp));
    }
}

}  // namespace

void install_cheats(Runtime &) {
    CheatState &s = state();
    s.godmode = on("CHEAT_GODMODE");
    s.ap = on("CHEAT_AP");
    if (!s.godmode && !s.ap) return;
    if (region() != Region::Europe) {
        std::cerr << "[vc2-cheats] VC2_CHEAT_GODMODE / VC2_CHEAT_AP: Europe only; off\n";
        return;
    }
    kernel().add_vblank_hook(&on_vblank);
    std::cerr << "[vc2-cheats] on:" << (s.godmode ? " godmode (HP 9999)" : "") << (s.ap ? " infinite AP" : "")
              << " - battle units, enemies included\n";
}

}  // namespace portablekit::vc2
