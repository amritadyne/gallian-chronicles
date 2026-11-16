// EXTRAS: "Unlock Extras" in place of Password (2026-11-16), as VC2's Enable
// Extras (profiles/vc2/host/vc2_extras.cpp): everything the game's passwords
// and data links unlock, through the game's own unlocking, messages and
// saving. Bonus Content and Save Conversion are left as they are.
//
// Addresses are VC3's own (both editions; analysis/claude dis.py on the
// executable), not VC2's through at().
//
// The EXTRAS screen ("this" below) runs, each frame (0x08B8C2EC), its list,
// then a Link Data flow (0x08B8A988, state at this+0xF8) while this+0xFC is
// set, a Password flow (0x08B8B274, state at this+0x104) while this+0x108 is
// set, and Save Conversion (0x08B8BCE8) while this+0x120 is set. The list's
// decide (around 0x08B8954C) sets this+0x108 for its first row (Password),
// opens the content list for the second (Bonus Content), sets this+0x120 for
// the third; Link Data, VC2's first row, is still in the code (an index of -1
// sets this+0xFC) but no row reaches it.
//
// Link Data starts the game's checker task "VlExtraContentsChecker" (through
// 0x0883A568, which starts a task by name), polls it (0x08B257CC: 1 = asked,
// 5 = done, 4 = failed), reads which of four sources were found (0x08B257F0,
// a bit each) and, for each, unlocks the content ids in its row of the table
// at 0x08CC2738 (four rows of 3 ids, a negative id ends a row), with the
// game's "%s has been unlocked." / "already unlocked" message for each.
//
// Here the Password flow is not run: Unlock Extras (the first row) runs Link
// Data instead, as many times as it takes, with the checker not started, its
// poll answering "asked" then "done", and the table filled each time with
// the next 12 ids of: the table's own (the data links' items) and every id
// of every password (the list the screen loaded at this+0x114, this+0x118
// entries of 48 bytes, up to 3 ids from +0x24, ending at a negative one).
// this+0x108 is kept at 2 while that runs (the decide sets 1), so the Password
// flow's hook knows a fresh choice from its own run.
//
// The row's texts are vc3-text-overrides.json's (MTPA_ADVERTISE 702030003,
// 702030006). VC3_NO_UNLOCK_EXTRAS=1 leaves EXTRAS as the game has it.

#include "vc3_extras.hpp"

#include "kernel/kernel.hpp"
#include "profile.hpp"
#include "psprecomp/corpus_abi.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace portablekit::vc3 {
namespace {

constexpr std::uint32_t kStartTask = 0x0883A568u;       // (name, a1, a2): starts a task by name
constexpr std::uint32_t kStartTaskInner = 0x0883A388u;  // (manager, 1000, name, a1, a2)
constexpr std::uint32_t kTaskManager = 0x08CF16D8u;     // the manager's pointer
constexpr std::uint32_t kCheckerStatus = 0x08B257CCu;
constexpr std::uint32_t kCheckerFound = 0x08B257F0u;
constexpr std::uint32_t kChecker = 0x08CF4A1Cu;         // the checker's pointer (status +0x304, found +0x30C)
constexpr std::uint32_t kLinkFlow = 0x08B8A988u;
constexpr std::uint32_t kPasswordFlow = 0x08B8B274u;
constexpr std::uint32_t kListOn = 0x08B892B8u;          // (this): the list takes input again
constexpr std::uint32_t kLinkTable = 0x08CC2738u;       // 4 x 3 ids
constexpr std::uint32_t kSources = 4u, kPerSource = 3u, kSlots = kSources * kPerSource;
// Return addresses in the Link Data flow.
constexpr std::uint32_t kStartFromLink = 0x08B8A9F0u;
constexpr std::uint32_t kStatusAsked = 0x08B8AA0Cu;  // waits for 1
constexpr std::uint32_t kStatusDone = 0x08B8AA5Cu;   // waits for 5 (4: failed)
constexpr std::uint32_t kFoundFromLink = 0x08B8AA90u;
// The screen's fields.
constexpr std::uint32_t kLinkState = 0xF8u, kLinkOn = 0xFCu, kPasswordOn = 0x108u;
constexpr std::uint32_t kPasswords = 0x114u, kPasswordCount = 0x118u;
constexpr std::uint32_t kPasswordSize = 48u, kPasswordIds = 0x24u;
constexpr std::uint32_t kOurRun = 2u;  // this+0x108 while ours runs (the decide sets 1)

psprecomp::GuestMemory &memory() { return portablekit::kernel().runtime().memory(); }

struct Extras {
    std::vector<std::int32_t> link_ids;  // the table's own ids, read before it is first filled
    std::vector<std::int32_t> ids;       // this run's
    std::size_t next = 0u;               // the first not yet put in the table
    std::uint32_t passes = 0u;
};
Extras &extras() {
    static Extras e;
    return e;
}

// 0x0883A568: as the game's, but the extra-content checker is not started
// when Link Data asks for it.
void start_task(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    auto &m = memory();
    const std::uint32_t name = ctx.gpr[4], a1 = ctx.gpr[5], a2 = ctx.gpr[6];
    if (ctx.gpr[31] == kStartFromLink && m.read_c_string(name, 32u) == "VlExtraContentsChecker") {
        ctx.set_gpr(2u, 0u);
        ctx.pc = ctx.gpr[31];
        return;
    }
    ctx.set_gpr(8u, a2);  // t0
    ctx.set_gpr(6u, name);
    ctx.set_gpr(7u, a1);
    ctx.set_gpr(4u, m.load32(kTaskManager));
    ctx.set_gpr(5u, 1000u);
    ctx.pc = kStartTaskInner;  // a tail call: it returns to our caller
}

std::uint32_t checker_field(std::uint32_t offset) {
    const std::uint32_t checker = memory().load32(kChecker);
    return checker != 0u ? memory().load32(checker + offset) : 0u;
}

void checker_status(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    const std::uint32_t back = ctx.gpr[31];
    std::uint32_t status = checker_field(0x304u);
    if (back == kStatusAsked) status = 1u;
    if (back == kStatusDone) status = 5u;
    ctx.set_gpr(2u, status);
    ctx.pc = back;
}

void read_link_ids() {
    Extras &e = extras();
    if (!e.link_ids.empty()) return;
    auto &m = memory();
    for (std::uint32_t source = 0; source < kSources; ++source)
        for (std::uint32_t i = 0; i < kPerSource; ++i) {
            const auto id = static_cast<std::int32_t>(m.load32(kLinkTable + (source * kPerSource + i) * 4u));
            if (id < 0) break;
            e.link_ids.push_back(id);
        }
}

// The run's ids: the data links' and every password's, each once.
void collect_ids(std::uint32_t self) {
    auto &m = memory();
    Extras &e = extras();
    read_link_ids();
    e.ids = e.link_ids;
    e.next = 0u, e.passes = 0u;
    const std::uint32_t list = m.load32(self + kPasswords), count = m.load32(self + kPasswordCount);
    std::size_t from_passwords = 0u;
    if (list != 0u && count <= 128u)  // its buffer holds 128 (0x1800 bytes)
        for (std::uint32_t p = 0; p < count; ++p)
            for (std::uint32_t i = 0; i < kPerSource; ++i) {
                const auto id = static_cast<std::int32_t>(m.load32(list + p * kPasswordSize + kPasswordIds + i * 4u));
                if (id < 0) break;
                if (std::find(e.ids.begin(), e.ids.end(), id) == e.ids.end()) e.ids.push_back(id), ++from_passwords;
            }
    std::cerr << "[vc3-extras] Unlock Extras: " << e.link_ids.size() << " data link items, " << from_passwords
              << " more from " << count << " passwords\n";
}

// The table filled with the next 12 ids; the sources that have one count as
// found.
void checker_found(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    const std::uint32_t back = ctx.gpr[31];
    std::uint32_t found = checker_field(0x30Cu);
    if (back == kFoundFromLink) {
        auto &m = memory();
        Extras &e = extras();
        read_link_ids();
        if (e.ids.empty()) e.ids = e.link_ids;  // reached some other way: the data links' own
        found = 0u;
        for (std::uint32_t slot = 0; slot < kSlots; ++slot) {
            const std::size_t at = e.next + slot;
            const std::int32_t id = at < e.ids.size() ? e.ids[at] : -1;
            m.store32(kLinkTable + slot * 4u, static_cast<std::uint32_t>(id));
            if (id >= 0) found |= 1u << (slot / kPerSource);
        }
        e.next = std::min(e.ids.size(), e.next + kSlots);
        ++e.passes;
    }
    ctx.set_gpr(2u, found);
    ctx.pc = back;
}

void start_pass(std::uint32_t self) {
    memory().store32(self + kLinkState, 0u);
    memory().store32(self + kLinkOn, 1u);
}

// 0x08B8B274 (this): the Password flow; here it runs Link Data until every
// id has been through it.
void password_flow(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    const std::uint32_t self = ctx.gpr[4];
    ctx.pc = ctx.gpr[31];
    auto &m = memory();
    Extras &e = extras();
    if (m.load32(self + kPasswordOn) != kOurRun) {  // chosen just now
        collect_ids(self);
        m.store32(self + kPasswordOn, kOurRun);
        if (!e.ids.empty()) {
            start_pass(self);
            return;
        }
    } else if (m.load32(self + kLinkOn) != 0u) {
        return;  // a pass is running (Link Data runs before us each frame)
    } else if (e.next < e.ids.size()) {
        start_pass(self);
        return;
    }
    // Done: the list was given back its input by Link Data's end; with
    // nothing to unlock it is given it here.
    m.store32(self + kPasswordOn, 0u);
    std::cerr << "[vc3-extras] Unlock Extras: " << e.ids.size() << " items in " << e.passes << " passes\n";
    if (e.passes == 0u)
        portablekit::kernel().call_guest(ctx, kListOn, GuestCallArguments{self}, [](AllegrexContext &, std::uint32_t) {});
}

// The words that relocation leaves alone (lui, relocated lw and jal skipped).
struct Check {
    std::uint32_t address;
    std::array<std::uint32_t, 8> words;
    std::array<bool, 8> compare;
};

bool code_is(Runtime &rt, const Check &c) {
    for (std::uint32_t i = 0; i < 8u; ++i) {
        if (!c.compare[i]) continue;
        if (!rt.memory().contains(c.address + i * 4u, 4u) || rt.memory().load32(c.address + i * 4u) != c.words[i]) return false;
    }
    return true;
}

}  // namespace

void install_extras(Runtime &rt) {
    if (const char *off = env("NO_UNLOCK_EXTRAS"); off != nullptr && std::strcmp(off, "1") == 0) {
        std::cerr << "[vc3-extras] EXTRAS as the game has it (VC3_NO_UNLOCK_EXTRAS)\n";
        return;
    }
    constexpr bool T = true, F = false;
    const Check checks[] = {
        {kStartTask, {0x27BDFFF0u, 0xAFBF0000u, 0x00C04025u, 0x00A03825u, 0x00803025u, 0u, 0u, 0u}, {T, T, T, T, T, F, F, F}},
        {kCheckerStatus, {0u, 0u, 0x10800003u, 0x00000000u, 0x10000002u, 0x8C820304u, 0x00001025u, 0x03E00008u},
         {F, F, T, T, T, T, T, T}},
        {kCheckerFound, {0u, 0u, 0x10800003u, 0x00000000u, 0x10000002u, 0x8C82030Cu, 0x00001025u, 0x03E00008u},
         {F, F, T, T, T, T, T, T}},
        {kLinkFlow, {0x27BDFFA0u, 0xAFB00038u, 0xAFB1003Cu, 0xAFB20040u, 0xAFB30044u, 0xAFB40048u, 0xAFB5004Cu, 0xAFBF0050u},
         {T, T, T, T, T, T, T, T}},
        {kPasswordFlow, {0x27BDFED0u, 0xAFB0010Cu, 0xAFB10110u, 0xAFB20114u, 0xAFB30118u, 0xAFB4011Cu, 0xAFBF0120u, 0x00808025u},
         {T, T, T, T, T, T, T, T}},
    };
    for (const Check &c : checks)
        if (!code_is(rt, c)) {
            std::cerr << "[vc3-extras] code at 0x" << std::hex << c.address << std::dec
                      << " is not as expected: EXTRAS left as the game has it\n";
            return;
        }
    rt.register_function(kStartTask, &start_task, "vc3_extras_start_task");
    rt.register_function(kCheckerStatus, &checker_status, "vc3_extras_checker_status");
    rt.register_function(kCheckerFound, &checker_found, "vc3_extras_checker_found");
    rt.register_function(kPasswordFlow, &password_flow, "vc3_extras_password_flow");
    std::cerr << "[vc3-extras] EXTRAS: Unlock Extras (Password) unlocks every data link and password item\n";
}

}  // namespace portablekit::vc3
