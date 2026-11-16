// EXTRAS: "Enable Bonus Content" in place of Link Data and Password (Europe).
//
// The EXTRAS screen (EU 0x08B8xxxx, its object "this" below) lists Link Data,
// Password and Content List. Each frame EU 0x08B88280 runs the list, or the
// Link Data flow (EU 0x08B86F9C, state at this+248) while this+252 is set, or
// the Password flow (EU 0x08B87848, state at this+260) while this+264 is set.
//
// Link Data starts the game's checker task "VlExtraContentsChecker" (through
// EU 0x08837DE0, which starts a task by name) to look for a Valkyria
// Chronicles (PS3) save and a Phantasy Star Portable 2 save, polls it (EU
// 0x08BA55B0: 1 = asked, 5 = done, 4 = failed) and reads which were found
// (EU 0x08BA55D4: bit 0 the first, bit 1 the second). For each found source it
// unlocks the content ids in its row of the table at EU 0x08CD6818 (two rows
// of 32 ids, a negative id ends a row), one "Unlocked %s." message each.
//
// Here the checker is not started, the poll answers "asked", then "done", and
// both sources count as found; the table's rows are first filled with every id
// the data links unlock and every id any password unlocks (the password list
// the screen loaded at this+276, this+280 entries of 164 bytes, the ids from
// +36, 32 at most, ending at a negative one). So the game's own flow unlocks
// all of it, with its own messages and saving.
//
// Password (the second row) opens the content list instead (the third row's
// steps, EU 0x08B85BD4), and the texts (vc2-text-overrides.json) call the rows
// "Enable Extras" and "Content List". The third row is left out: the menu
// builder (EU 0x08B882D8) makes three row panels and hangs each on the list
// with EU 0x08B0CA24 (list, row) (at 0x08B88564, the row number in s2); the
// third is not hung on it (so not counted) and its drawn part (its method at
// +24) is hidden (flag 0x400 at +24, as the list hides rows out of view), or
// its empty box stays.
//
// OPTIONS: the Preferences tab's Installed Data row (the PSP's data install,
// of no use here) is left off its list the same way (VC2_KEEP_INSTALL=1 keeps
// it, as it keeps the title menu's INSTALL).
//
// VC2_NO_BONUS_CONTENT=1 leaves EXTRAS as it was.

#include "vc2_extras.hpp"
#include "vc2_audio.hpp"
#include "vc2_region.hpp"

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

namespace portablekit::vc2 {
namespace {

constexpr std::uint32_t kEuStartTask = 0x08837DE0u;         // (name, a1, a2): starts a task by name
constexpr std::uint32_t kEuStartTaskInner = 0x08837C00u;    // (manager, 1000, name, a1, a2)
constexpr std::uint32_t kEuTaskManager = 0x08CF99E0u;       // the manager's pointer
constexpr std::uint32_t kEuCheckerStatus = 0x08BA55B0u;
constexpr std::uint32_t kEuCheckerFound = 0x08BA55D4u;
constexpr std::uint32_t kEuChecker = 0x08CFCC94u;           // the checker's pointer (status +772, found +780)
constexpr std::uint32_t kEuPasswordFlow = 0x08B87848u;
constexpr std::uint32_t kEuLinkTable = 0x08CD6818u;         // 2 x 32 ids
// Return addresses in the Link Data flow (EU 0x08B86F9C).
constexpr std::uint32_t kEuStartFromLink = 0x08B87000u;
constexpr std::uint32_t kEuStatusAsked = 0x08B87018u;  // waits for 1
constexpr std::uint32_t kEuStatusDone = 0x08B87064u;   // waits for 5
constexpr std::uint32_t kEuFoundFromLink = 0x08B87094u;
// The content list's steps (EU 0x08B85BD4).
constexpr std::uint32_t kEuLayer = 0x08AFDA78u;        // (this+64, n): layer n
constexpr std::uint32_t kEuOpenContentList = 0x08B85968u;
constexpr std::uint32_t kEuUiStepA = 0x08AFA0D4u;
constexpr std::uint32_t kEuUiStepB = 0x08AFB7F8u;

constexpr std::uint32_t kStatusAsked = 1u, kStatusDone = 5u, kBothFound = 3u;
constexpr std::uint32_t kEuAttachRow = 0x08B0CA24u;      // (list, row)
constexpr std::uint32_t kEuSetParent = 0x08AFF54Cu;      // (child, parent, 0, 0)
constexpr std::uint32_t kEuAttachFromExtras = 0x08B8856Cu;  // its return address in the EXTRAS builder
// The OPTIONS screen's Preferences tab (builder around EU 0x08B40A18) makes one
// row per entry of the option table (EU 0x08CD0698, 40 bytes an entry; 0-5 on
// this tab, fp the entry) and hangs each on its list at 0x08B40C4C. Entry 5 is
// Installed Data (message 704040009), which has no use on Windows.
constexpr std::uint32_t kEuAttachFromOptions = 0x08B40C54u;
constexpr std::uint32_t kInstalledDataEntry = 5u;
constexpr std::uint32_t kHidden = 0x400u;                   // a widget's flags (+24): not drawn

psprecomp::GuestMemory &memory() { return portablekit::kernel().runtime().memory(); }

struct Extras {
    std::vector<std::int32_t> link_ids;  // the table's own ids, read once
    bool told_fill = false;
};
Extras &extras() {
    static Extras e;
    return e;
}

// EU 0x08837DE0: as the game's, but the extra-content checker is not started
// when Link Data asks for it.
void start_task(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    auto &m = memory();
    const std::uint32_t name = ctx.gpr[4], a1 = ctx.gpr[5], a2 = ctx.gpr[6];
    if (ctx.gpr[31] == at(kEuStartFromLink) && m.read_c_string(name, 32u) == "VlExtraContentsChecker") {
        std::cerr << "[vc2-extras] Enable Extras: no data link check, everything counts as linked\n";
        ctx.set_gpr(2u, 0u);
        ctx.pc = ctx.gpr[31];
        return;
    }
    ctx.set_gpr(8u, a2);   // t0
    ctx.set_gpr(6u, name);
    ctx.set_gpr(7u, a1);
    ctx.set_gpr(4u, m.load32(at(kEuTaskManager)));
    ctx.set_gpr(5u, 1000u);
    ctx.pc = at(kEuStartTaskInner);  // a tail call: it returns to our caller
}

std::uint32_t checker_field(std::uint32_t offset) {
    const std::uint32_t checker = memory().load32(at(kEuChecker));
    return checker != 0u ? memory().load32(checker + offset) : 0u;
}

void checker_status(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    const std::uint32_t back = ctx.gpr[31];
    std::uint32_t status = checker_field(772u);
    if (back == at(kEuStatusAsked)) status = kStatusAsked;
    if (back == at(kEuStatusDone)) status = kStatusDone;
    ctx.set_gpr(2u, status);
    ctx.pc = back;
}

// Both table rows filled with every data link and password id.
void fill_link_table(std::uint32_t self) {
    auto &m = memory();
    Extras &e = extras();
    if (e.link_ids.empty())
        for (std::uint32_t row = 0; row < 2u; ++row)
            for (std::uint32_t i = 0; i < 32u; ++i) {
                const auto id = static_cast<std::int32_t>(m.load32(at(kEuLinkTable) + (row * 32u + i) * 4u));
                if (id < 0) break;
                e.link_ids.push_back(id);
            }
    std::vector<std::int32_t> ids = e.link_ids;
    const std::uint32_t list = m.load32(self + 276u), count = m.load32(self + 280u);
    std::size_t from_passwords = 0u;
    if (list != 0u && count <= 512u)
        for (std::uint32_t p = 0; p < count; ++p)
            for (std::uint32_t i = 0; i < 32u; ++i) {
                const auto id = static_cast<std::int32_t>(m.load32(list + p * 164u + 36u + i * 4u));
                if (id < 0) break;
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id), ++from_passwords;
            }
    if (ids.size() > 64u)
        std::cerr << "[vc2-extras] " << ids.size() << " ids, the table holds 64: select it again for the rest\n";
    // Ones not yet unlocked first would need the game's own check; all go in,
    // the game says "already unlocked" for those that are.
    for (std::uint32_t slot = 0; slot < 64u; ++slot) {
        const std::int32_t id = slot < ids.size() ? ids[slot] : -1;
        m.store32(at(kEuLinkTable) + slot * 4u, static_cast<std::uint32_t>(id));
    }
    if (!e.told_fill) {
        e.told_fill = true;
        std::cerr << "[vc2-extras] bonus content: " << e.link_ids.size() << " data link ids, " << from_passwords
                  << " more from " << count << " passwords\n";
    }
}

void checker_found(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    const std::uint32_t back = ctx.gpr[31];
    std::uint32_t found = checker_field(780u);
    if (back == at(kEuFoundFromLink)) {
        fill_link_table(ctx.gpr[16]);  // s0: the EXTRAS screen
        found = kBothFound;
    }
    ctx.set_gpr(2u, found);
    ctx.pc = back;
}

// Hides a row's drawn part (the row's method at +24), with flag 0x400 as the
// list (EU 0x08B0CD68) hides rows scrolled out of view.
void hide_drawn_part(AllegrexContext &ctx, std::uint32_t row, const char *what) {
    auto &m = memory();
    const std::uint32_t entry = m.load32(row + 8u) + 24u;
    const auto adjust = static_cast<std::int16_t>(m.load16(entry));
    portablekit::kernel().call_guest(ctx, m.load32(entry + 4u), GuestCallArguments{row + static_cast<std::uint32_t>(adjust)},
                                     [what](AllegrexContext &, std::uint32_t drawn) {
                                         if (drawn == 0u) return;
                                         memory().store32(drawn + 24u, memory().load32(drawn + 24u) | kHidden);
                                         std::cerr << "[vc2-extras] " << what << "\n";
                                     });
}

struct RowRules {
    bool extras_third = true;    // EXTRAS: the old third row (VC2_EXTRAS_THREE_ROWS=1 keeps it)
    bool installed_data = true;  // OPTIONS: Installed Data (VC2_KEEP_INSTALL=1 keeps it)
};
RowRules &row_rules() {
    static RowRules r;
    return r;
}

// EU 0x08B0CA24 (list, row): as the game's (the row's parent is the list, then
// the list lays itself out, its method at +200), except for two rows: the
// EXTRAS builder's third, and OPTIONS' Installed Data. Those are not hung on
// the list, so the cursor never reaches them.
void attach_row(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    const std::uint32_t list = ctx.gpr[4], row = ctx.gpr[5];
    const std::uint32_t back = ctx.gpr[31];
    ctx.pc = back;
    const RowRules &rules = row_rules();
    // The OPTIONS screen is open: its volume changes are the player's.
    if (back == at(kEuAttachFromOptions)) audio_options_opened();
    if (rules.extras_third && back == at(kEuAttachFromExtras) && ctx.gpr[18] == 2u) {  // s2: the row number
        // Not on the list, it is still drawn where the layout puts it.
        hide_drawn_part(ctx, row, "EXTRAS: third row left out and hidden");
        return;
    }
    if (rules.installed_data && back == at(kEuAttachFromOptions) && ctx.gpr[30] == kInstalledDataEntry) {  // fp: the entry
        // Made by the code, not the layout: it goes on the list's own parent,
        // hidden, so it is freed with the screen.
        const std::uint32_t parent = memory().load32(list + 4u);
        if (parent == 0u) {
            std::cerr << "[vc2-extras] OPTIONS: Installed Data left out\n";
            return;
        }
        portablekit::kernel().call_guest(ctx, at(kEuSetParent), GuestCallArguments{row, parent, 0u, 0u},
                                         [row](AllegrexContext &c, std::uint32_t) {
                                             hide_drawn_part(c, row, "OPTIONS: Installed Data left out");
                                         });
        return;
    }
    portablekit::kernel().call_guest(ctx, at(kEuSetParent), GuestCallArguments{row, list, 0u, 0u}, [list](AllegrexContext &c, std::uint32_t) {
        auto &m = memory();
        const std::uint32_t entry = m.load32(list + 8u) + 200u;
        const auto adjust = static_cast<std::int16_t>(m.load16(entry));
        portablekit::kernel().call_guest(c, m.load32(entry + 4u), GuestCallArguments{list + static_cast<std::uint32_t>(adjust)},
                                         [](AllegrexContext &, std::uint32_t) {});
    });
}

// EU 0x08B87848 (this): the Password flow; here it opens the content list.
void password_flow(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    const std::uint32_t self = ctx.gpr[4];
    ctx.pc = ctx.gpr[31];
    auto &m = memory();
    m.store32(self + 264u, 0u);
    m.store32(self + 100u, 1u);
    std::cerr << "[vc2-extras] Content List (the second row)\n";
    auto &k = portablekit::kernel();
    k.call_guest(ctx, at(kEuLayer), GuestCallArguments{self + 64u, 0u}, [self](AllegrexContext &c, std::uint32_t layer) {
        portablekit::kernel().call_guest(c, at(kEuOpenContentList), GuestCallArguments{self, layer}, [](AllegrexContext &c2, std::uint32_t) {
            portablekit::kernel().call_guest(c2, at(kEuUiStepA), GuestCallArguments{}, [](AllegrexContext &c3, std::uint32_t) {
                portablekit::kernel().call_guest(c3, at(kEuUiStepB), GuestCallArguments{}, [](AllegrexContext &c4, std::uint32_t) {
                    c4.set_gpr(2u, 1u);
                });
            });
        });
    });
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

namespace {
bool env_is_1(const char *name) {
    const char *value = env(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}
}  // namespace

void install_extras(Runtime &rt) {
    const bool bonus = !env_is_1("NO_BONUS_CONTENT");
    RowRules &rules = row_rules();
    rules.extras_third = bonus && !env_is_1("EXTRAS_THREE_ROWS");
    rules.installed_data = !env_is_1("KEEP_INSTALL");
    if (!bonus) std::cerr << "[vc2-extras] EXTRAS as the game has it (VC2_NO_BONUS_CONTENT)\n";
    const Check attach{at(kEuAttachRow), {0x27BDFFF0u, 0xAFB00000u, 0x00808025u, 0x00A02025u, 0x02002825u, 0x00003025u, 0xAFBF0004u, 0u},
                       {true, true, true, true, true, true, true, false}};
    if (rules.extras_third || rules.installed_data) {
        if (code_is(rt, attach))
            rt.register_function(at(kEuAttachRow), &attach_row, "vc2_extras_attach_row");
        else
            std::cerr << "[vc2-extras] code at 0x" << std::hex << at(kEuAttachRow) << std::dec
                      << " is not as expected: rows left as the game has them\n";
    }
    if (!bonus) return;
    const Check checks[] = {
        {at(kEuStartTask), {0x27BDFFF0u, 0x00C04025u, 0x00803025u, 0u, 0x00A03825u, 0u, 0xAFBF0000u, 0u},
         {true, true, true, false, true, false, true, false}},
        {at(kEuCheckerStatus), {0u, 0u, 0x10800003u, 0x00000000u, 0x10000002u, 0x8C820304u, 0x00001025u, 0x03E00008u},
         {false, false, true, true, true, true, true, true}},
        {at(kEuCheckerFound), {0u, 0u, 0x10800003u, 0x00000000u, 0x10000002u, 0x8C82030Cu, 0x00001025u, 0x03E00008u},
         {false, false, true, true, true, true, true, true}},
        {at(kEuPasswordFlow), {0x27BDFE60u, 0xAFB00180u, 0xAFB10184u, 0xAFB20188u, 0xAFB3018Cu, 0xAFB40190u, 0xAFBF0194u, 0u},
         {true, true, true, true, true, true, true, false}},
    };
    for (const Check &c : checks)
        if (!code_is(rt, c)) {
            std::cerr << "[vc2-extras] code at 0x" << std::hex << c.address << std::dec
                      << " is not as expected: EXTRAS left as the game has it\n";
            return;
        }
    rt.register_function(at(kEuStartTask), &start_task, "vc2_extras_start_task");
    rt.register_function(at(kEuCheckerStatus), &checker_status, "vc2_extras_checker_status");
    rt.register_function(at(kEuCheckerFound), &checker_found, "vc2_extras_checker_found");
    rt.register_function(at(kEuPasswordFlow), &password_flow, "vc2_extras_password_flow");
    std::cerr << "[vc2-extras] EXTRAS: Enable Extras (Link Data) unlocks every data link and password item; "
                 "Password opens the content list\n";
}

}  // namespace portablekit::vc2
