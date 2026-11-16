// Frame census: what the game changes every frame, and which code changes it.
// The groundwork for high frame rates (plan Phase 6): VC2 has no time step
// (see vc3_tick.cpp), so every system that moves things, counts down or
// animates does it by a fixed amount per frame. This finds them.
//
// VC3_CENSUS=<folder> turns it on; F10 in game starts a census (or
// VC3_CENSUS_AT=<game frame> starts one by itself). Two phases:
//
//  1. Change census, VC3_CENSUS_FRAMES game frames (default 120): user RAM
//     (0x08804000-0x0A000000) is compared word by word at every frame start.
//     A word that changes in at least 80% of the frames is frame-driven and
//     is classed by how it changes: an integer stepping by +1 or -1 a frame
//     (frame counters, countdown timers), an integer stepping by another fixed
//     amount, a float moving by a steady amount (linear motion, float timers),
//     a float that varies (eased motion, physics, animation), or other.
//     Nearby words are grouped into blocks (objects). Report:
//     <folder>/census-<n>.txt.
//  2. Attribution, VC3_CENSUS_ATTRIBUTE_FRAMES game frames (default 2): the
//     busiest frame-driven words (up to 8192) are compared again around every
//     call from one generated code unit into another, and each change is
//     credited to the guest function that was running: the innermost one
//     called across units (calls inside a 16 KiB unit are not seen, so a
//     change can belong to that function's own callees). Needs the runtime's
//     call observers, which the optimised build compiles out
//     (PSPRECOMP_AOT_PRODUCTION_FASTPATHS); "Play VC2 (frame census).cmd"
//     builds out\vc2-trace with them. Without them the report says so.
//     Report: <folder>/census-<n>-functions.txt.
//
// F7 writes user RAM (0x08800000-0x0A000000, 24 MiB) to <folder>/ram-<n>.bin:
// the game's decrypted text, among the rest, while a message is on screen.
//
// F11 starts a swap hunt instead (level-of-detail swaps and other small
// switches): see "Swap hunt" below.
//
// A game frame starts when Kurofune's frame start (EU 0x08BEBC54) has drained
// KfVSyncSema: its last sceKernelPollSema on that semaphore fails.

#include "vc3_census.hpp"
#include "vc3_region.hpp"
#include "vc3_tick.hpp"

#include "profile.hpp"
#include "kernel/kernel.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(PORTABLEKIT_HAS_SDL)
#include <SDL3/SDL.h>
#endif

namespace portablekit::vc3 {
namespace {

constexpr std::uint32_t kEuVsyncBlock = 0x08D61E40u;
constexpr std::uint32_t kPollSemaNid = 0x58B1F937u;
constexpr std::uint32_t kRamStart = 0x08804000u;
constexpr std::uint32_t kRamEnd = 0x0A000000u;
constexpr std::uint32_t kWords = (kRamEnd - kRamStart) / 4u;
// The executable's code and static data (EU): above it, heap and stacks.
constexpr std::uint32_t kEuStaticEnd = 0x08D7D000u;
constexpr std::size_t kMaxWatchWords = 8192u;
constexpr std::uint64_t kMaxAttributionEvents = 4'000'000u;

enum Class : std::uint8_t { CountStep, IntStep, FloatSteady, FloatVarying, Other, kClasses };
constexpr const char *kClassNames[kClasses] = {"count +-1/frame", "int fixed step", "float steady step",
                                               "float varying", "other"};
constexpr const char *kClassShort[kClasses] = {"count", "istep", "fsteady", "fvary", "other"};

bool plausible_float(std::uint32_t bits) {
    if (bits == 0u || bits == 0x80000000u) return true;
    const std::uint32_t exponent = (bits >> 23u) & 0xFFu;
    return exponent >= 127u - 24u && exponent <= 127u + 24u;
}
float as_float(std::uint32_t bits) { return std::bit_cast<float>(bits); }

struct Word {
    std::uint32_t address;
    Class kind;
    std::uint16_t changes;
    std::uint32_t value;
    std::int32_t int_step;
    float float_step;
};

struct Block {
    std::uint32_t start = 0u, end = 0u;  // [start, end)
    std::array<std::uint32_t, kClasses> counts{};
    std::vector<std::size_t> words;      // indices into Census::words
    std::size_t total() const {
        std::size_t n = 0u;
        for (auto c : counts) n += c;
        return n;
    }
};

struct FunctionStats {
    std::uint64_t changes = 0u;
    std::array<std::uint32_t, kClasses> counts{};
    std::vector<std::uint32_t> samples;              // first few addresses
    std::map<std::uint32_t, std::uint32_t> callers;  // caller function -> changes
};

struct Census {
    std::filesystem::path folder;
    std::uint32_t frames_wanted = 120u;
    std::uint32_t attribute_frames = 2u;
    std::int64_t start_at = -1;
    std::uint64_t game_frames = 0u;
    std::uint32_t runs = 0u;
    bool key_down = false;

    enum class Phase { Idle, Diff, Attribute, Hunt, HuntAttribute, Control } phase = Phase::Idle;
    // Control hunt (F8).
    bool control_key_down = false;
    std::uint32_t controls = 0u;
    std::vector<std::uint16_t> changed_active, changed_idle, pad_match;
    std::uint32_t active_frames = 0u, idle_frames = 0u;
    std::map<std::uint32_t, std::uint32_t> held_masks;  // buttons held -> frames
    bool hunt_key_down = false;
    bool mark_key_down = false;
    std::uint32_t marks = 0u;
    std::vector<std::uint32_t> last_step;  // frame of the word's last uncredited clean step
    std::vector<std::uint8_t> hits;        // clean steps within a second before an F12 mark
    std::vector<std::uint32_t> last_change;  // frame of the word's last uncredited change of any kind
    std::vector<std::uint8_t> any_hits;      // changes of any kind within a second before a mark
    std::uint32_t auto_marks = 0u;           // marks made by vc3_native_gu's texture watch
    std::vector<double> frame_time;        // seconds since the hunt began, per hunt frame
    std::uint32_t hunts = 0u;
    double hunt_seconds = 20.0;
    std::chrono::steady_clock::time_point phase_start{};
    std::vector<std::uint16_t> stable;   // frames since the word last changed (saturating)
    std::vector<std::uint8_t> toggles;   // clean steps between small values
    std::vector<std::uint32_t> watch_changes;
    std::string attribution_name = "census";
    std::uint32_t frames_done = 0u;
    std::vector<std::uint32_t> previous, current;
    std::vector<std::int32_t> int_step;
    std::vector<float> float_step;
    std::vector<std::uint16_t> changes;
    std::vector<std::uint8_t> flags;  // 1: seen a change, 2: integer step constant, 4: float step constant

    std::vector<Word> words;
    std::vector<Block> blocks;

    // Attribution.
    std::vector<std::uint32_t> watch_address, watch_value;
    std::vector<std::uint8_t> watch_class;
    std::unordered_map<std::int32_t, std::vector<std::uint32_t>> stacks;
    std::int32_t thread = 0;
    std::uint64_t events = 0u, chained_events = 0u;
    std::unordered_map<std::uint32_t, FunctionStats> functions;
    bool hooks_on = false;
    bool wobbled = false;  // the step alternated during the comparison (vc3_tick)
};

Census &census() {
    static Census state;
    return state;
}

bool key_pressed(int scancode) {
#if defined(PORTABLEKIT_HAS_SDL)
    int count = 0;
    const bool *keys = SDL_GetKeyboardState(&count);
    return keys != nullptr && scancode < count && keys[scancode];
#else
    (void)scancode;
    return false;
#endif
}

bool f10_pressed() {
#if defined(PORTABLEKIT_HAS_SDL)
    int count = 0;
    const bool *keys = SDL_GetKeyboardState(&count);
    return keys != nullptr && SDL_SCANCODE_F10 < count && keys[SDL_SCANCODE_F10];
#else
    return false;
#endif
}

std::string hex8(std::uint32_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return text.str();
}

void read_ram(Runtime &rt, std::vector<std::uint32_t> &into) {
    into.resize(kWords);
    rt.memory().copy_out(kRamStart, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(into.data()),
                                                            static_cast<std::size_t>(kWords) * 4u));
}

void start_diff(Runtime &rt) {
    Census &c = census();
    c.phase = Census::Phase::Diff;
    c.frames_done = 0u;
    read_ram(rt, c.previous);
    c.int_step.assign(kWords, 0);
    c.float_step.assign(kWords, 0.0f);
    c.changes.assign(kWords, 0u);
    c.flags.assign(kWords, 0u);
    c.words.clear();
    c.blocks.clear();
    set_step_wobble(true);
    std::cerr << "[vc3-census] census " << c.runs << ": comparing RAM over the next " << c.frames_wanted
              << " game frames (the game slows down while it runs)\n";
}

void diff_frame(Runtime &rt) {
    Census &c = census();
    read_ram(rt, c.current);
    for (std::uint32_t i = 0; i < kWords; ++i) {
        const std::uint32_t now = c.current[i], before = c.previous[i];
        if (now == before) continue;
        if (c.changes[i] != 0xFFFFu) ++c.changes[i];
        const std::int32_t step = static_cast<std::int32_t>(now - before);
        const float fstep = as_float(now) - as_float(before);
        std::uint8_t &f = c.flags[i];
        if ((f & 1u) == 0u) {
            f = 1u | 2u | 4u;
            c.int_step[i] = step;
            c.float_step[i] = fstep;
        } else {
            if (step != c.int_step[i]) f &= static_cast<std::uint8_t>(~2u);
            const float last = c.float_step[i];
            if (!(std::fabs(fstep - last) <= std::max(1e-6f, std::fabs(last) * 1e-3f)))
                f &= static_cast<std::uint8_t>(~4u);
        }
    }
    c.previous.swap(c.current);
    ++c.frames_done;
}

Class classify(std::uint32_t i) {
    const Census &c = census();
    const std::uint8_t f = c.flags[i];
    const std::uint32_t value = c.previous[i];
    if ((f & 2u) != 0u && (c.int_step[i] == 1 || c.int_step[i] == -1)) return CountStep;
    const bool is_float = plausible_float(value) && std::isfinite(c.float_step[i]);
    // A small integer stepping steadily is an integer; a float stepping
    // steadily by a whole-number bit pattern is still a float.
    if ((f & 2u) != 0u && !(is_float && value > 0x00FFFFFFu)) return IntStep;
    if (is_float) return (f & 4u) != 0u ? FloatSteady : FloatVarying;
    return Other;
}

void finish_diff() {
    Census &c = census();
    c.wobbled = step_wobble();
    set_step_wobble(false);
    const std::uint32_t intervals = std::max<std::uint32_t>(c.frames_done, 1u);
    const std::uint32_t threshold = std::max<std::uint32_t>(1u, intervals * 4u / 5u);
    for (std::uint32_t i = 0; i < kWords; ++i) {
        if (c.changes[i] < threshold) continue;
        const Class kind = classify(i);
        c.words.push_back(Word{kRamStart + i * 4u, kind, c.changes[i], c.previous[i], c.int_step[i], c.float_step[i]});
    }
    // Blocks: frame-driven words no more than 16 bytes apart.
    for (std::size_t w = 0; w < c.words.size(); ++w) {
        const Word &word = c.words[w];
        if (c.blocks.empty() || word.address > c.blocks.back().end + 16u) c.blocks.push_back(Block{word.address, word.address, {}, {}});
        Block &block = c.blocks.back();
        block.end = word.address + 4u;
        ++block.counts[word.kind];
        block.words.push_back(w);
    }
    // Free the per-word arrays.
    std::vector<std::int32_t>().swap(c.int_step);
    std::vector<float>().swap(c.float_step);
    std::vector<std::uint16_t>().swap(c.changes);
    std::vector<std::uint8_t>().swap(c.flags);
    std::vector<std::uint32_t>().swap(c.current);
}

// At a corrected high frame rate: how many game steps a per-frame change is.
// A whole multiple of 1/step (2.75 at 165 Hz) means it ignores the step.
void step_multiple(std::ostream &out, double per_frame) {
    const float step = corrected_step();
    if (step <= 0.0f || per_frame == 0.0) return;
    const double per_step = per_frame / step;
    out << " (" << std::setprecision(4) << per_step << " per game step"
        << (std::fabs(std::fabs(per_frame) - std::round(std::fabs(per_frame))) < 1e-4 && std::fabs(per_frame) >= 1.0
                ? ", FRAME-LOCKED?"
                : "")
        << ")" << std::setprecision(6);
}

void describe_word(std::ostream &out, const Word &w) {
    out << "    " << hex8(w.address) << "  " << std::left << std::setw(8) << kClassShort[w.kind] << std::right;
    switch (w.kind) {
    case CountStep:
    case IntStep:
        out << " value " << static_cast<std::int32_t>(w.value) << ", " << (w.int_step >= 0 ? "+" : "") << w.int_step
            << " a frame";
        step_multiple(out, w.int_step);
        break;
    case FloatSteady:
        out << " value " << as_float(w.value) << ", " << (w.float_step >= 0.0f ? "+" : "") << w.float_step
            << " a frame";
        step_multiple(out, w.float_step);
        break;
    case FloatVarying: out << " value " << as_float(w.value) << ", last step " << w.float_step; break;
    default: out << " value " << hex8(w.value); break;
    }
    out << "\n";
}

void write_census_report() {
    Census &c = census();
    std::error_code error;
    std::filesystem::create_directories(c.folder, error);
    const std::filesystem::path path = c.folder / ("census-" + std::to_string(c.runs) + ".txt");
    std::ofstream out(path);
    std::array<std::size_t, kClasses> totals{};
    for (const Word &w : c.words) ++totals[w.kind];
    const std::uint32_t static_end = at(kEuStaticEnd) != 0u ? at(kEuStaticEnd) : kEuStaticEnd;
    out << "VC2 frame census " << c.runs << " (" << region_name() << "), game frame " << c.game_frames << ", "
        << c.frames_done << " frames compared";
    if (corrected_step() > 0.0f) out << ", game step corrected to " << corrected_step() << " of a PSP frame";
    out << "\n";
    if (c.wobbled)
        out << "The step alternated x1 / x1.5 frame by frame during the comparison: what follows the step changed by\n"
               "alternating amounts, so EVERY steady change below (count, istep, fsteady) IGNORES THE STEP.\n";
    out << "Frame-driven words (changed in at least 80% of frames): " << c.words.size() << " in " << c.blocks.size()
        << " blocks\n";
    for (int k = 0; k < kClasses; ++k) out << "  " << std::left << std::setw(18) << kClassNames[k] << std::right << totals[k] << "\n";
    out << "\nBlocks by size (static = the executable's own data, else heap/stack):\n";
    std::vector<std::size_t> order(c.blocks.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return c.blocks[a].total() > c.blocks[b].total(); });
    std::size_t shown = 0u;
    for (const std::size_t b : order) {
        if (++shown > 400u) break;
        const Block &block = c.blocks[b];
        out << "\n" << hex8(block.start) << "-" << hex8(block.end) << " (" << (block.end - block.start) << " bytes, "
            << (block.start < static_end ? "static" : "heap") << "):";
        for (int k = 0; k < kClasses; ++k)
            if (block.counts[k] != 0u) out << " " << kClassShort[k] << " " << block.counts[k];
        out << "\n";
        std::size_t listed = 0u;
        for (const std::size_t w : block.words) {
            if (++listed > 12u) {
                out << "    ... " << (block.words.size() - 12u) << " more\n";
                break;
            }
            describe_word(out, c.words[w]);
        }
    }
    std::cerr << "[vc3-census] census " << c.runs << ": " << c.words.size() << " frame-driven words in "
              << c.blocks.size() << " blocks; report " << path.string() << "\n";
}

// --- Attribution -----------------------------------------------------------------

void check_watch(Runtime &rt, std::uint32_t function, std::uint32_t caller) {
    Census &c = census();
    ++c.events;
    auto &memory = rt.memory();
    for (std::size_t i = 0; i < c.watch_address.size(); ++i) {
        const std::uint32_t now = memory.aot_load32(c.watch_address[i]);
        if (now == c.watch_value[i]) continue;
        c.watch_value[i] = now;
        if (i < c.watch_changes.size()) ++c.watch_changes[i];
        FunctionStats &stats = c.functions[function];
        ++stats.changes;
        ++stats.counts[c.watch_class[i]];
        if (stats.samples.size() < 8u &&
            std::find(stats.samples.begin(), stats.samples.end(), c.watch_address[i]) == stats.samples.end())
            stats.samples.push_back(c.watch_address[i]);
        if (caller != 0u) ++stats.callers[caller];
    }
}

std::vector<std::uint32_t> &stack() { return census().stacks[census().thread]; }
std::uint32_t top(std::size_t from_end = 0u) {
    const auto &s = stack();
    return s.size() > from_end ? s[s.size() - 1u - from_end] : 0u;
}

void on_pre_dispatch(Runtime &rt, AllegrexContext &, std::uint32_t pc, std::int32_t uid) {
    Census &c = census();
    if (!c.hooks_on) return;
    check_watch(rt, top(), top(1));  // what ran before this dispatch (host code: 0)
    c.thread = uid;
    auto &s = stack();
    s.clear();
    s.push_back(pc);
}
void on_post_dispatch(Runtime &rt, AllegrexContext &, std::uint32_t, std::int32_t) {
    if (!census().hooks_on) return;
    check_watch(rt, top(), top(1));
    stack().clear();
}
void on_pre_call(Runtime &rt, AllegrexContext &, std::uint32_t target, std::uint32_t) {
    Census &c = census();
    if (!c.hooks_on || c.events > kMaxAttributionEvents) return;
    ++c.chained_events;
    check_watch(rt, top(), top(1));
    stack().push_back(target);
}
void on_post_call(Runtime &rt, AllegrexContext &, std::uint32_t, std::uint32_t) {
    Census &c = census();
    if (!c.hooks_on || c.events > kMaxAttributionEvents) return;
    check_watch(rt, top(), top(1));
    if (!stack().empty()) stack().pop_back();
}

void start_attribution(Runtime &rt) {
    Census &c = census();
    // The busiest blocks first, word by word, up to the watch limit.
    std::vector<std::size_t> order(c.blocks.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return c.blocks[a].total() > c.blocks[b].total(); });
    c.watch_address.clear();
    c.watch_class.clear();
    // Steady changers first (under the step wobble, the frame-locked ones),
    // then the busiest blocks' others.
    for (const std::size_t b : order)
        for (const std::size_t w : c.blocks[b].words) {
            const Class kind = c.words[w].kind;
            if (kind != CountStep && kind != IntStep && kind != FloatSteady) continue;
            if (c.watch_address.size() >= kMaxWatchWords) break;
            c.watch_address.push_back(c.words[w].address);
            c.watch_class.push_back(kind);
        }
    for (const std::size_t b : order) {
        for (const std::size_t w : c.blocks[b].words) {
            if (c.watch_address.size() >= kMaxWatchWords) break;
            const Class kind = c.words[w].kind;
            if (kind == CountStep || kind == IntStep || kind == FloatSteady) continue;  // already in
            c.watch_address.push_back(c.words[w].address);
            c.watch_class.push_back(c.words[w].kind);
        }
        if (c.watch_address.size() >= kMaxWatchWords) break;
    }
    c.watch_value.resize(c.watch_address.size());
    for (std::size_t i = 0; i < c.watch_address.size(); ++i) c.watch_value[i] = rt.memory().aot_load32(c.watch_address[i]);
    c.functions.clear();
    c.stacks.clear();
    c.events = c.chained_events = 0u;
    c.phase = Census::Phase::Attribute;
    c.frames_done = 0u;
    c.hooks_on = true;
    psprecomp::set_runtime_pre_dispatch_hook(&on_pre_dispatch);
    psprecomp::set_runtime_post_dispatch_hook(&on_post_dispatch);
    psprecomp::set_runtime_pre_chained_call_hook(&on_pre_call);
    psprecomp::set_runtime_post_chained_call_hook(&on_post_call);
    std::cerr << "[vc3-census] census " << c.runs << ": crediting changes to " << c.watch_address.size()
              << " words over " << c.attribute_frames << " game frames\n";
}

void write_attribution(bool final);

void finish_attribution() {
    Census &c = census();
    c.hooks_on = false;
    psprecomp::set_runtime_pre_dispatch_hook(nullptr);
    psprecomp::set_runtime_post_dispatch_hook(nullptr);
    psprecomp::set_runtime_pre_chained_call_hook(nullptr);
    psprecomp::set_runtime_post_chained_call_hook(nullptr);
    write_attribution(true);
}

// Also written every few seconds while a swap hunt's attribution runs, so
// quitting early still leaves a report.
void write_attribution(bool final) {
    Census &c = census();
    const std::uint32_t number = c.attribution_name == "census" ? c.runs : c.hunts;
    const std::filesystem::path path = c.folder / (c.attribution_name + "-" + std::to_string(number) + "-functions.txt");
    std::ofstream out(path);
    out << "VC2 " << (c.attribution_name == "hunt" ? "swap hunt " : c.attribution_name == "control" ? "control hunt " : "frame census ") << number
        << ": which functions change the watched words (" << c.frames_done
        << " game frames, " << c.watch_address.size() << " words watched, " << c.events << " checks, "
        << c.chained_events << " cross-unit calls seen)\n";
    if (c.chained_events == 0u) {
        out << "\nNo cross-unit calls were seen: this build compiles the runtime's call observers out\n"
               "(PSPRECOMP_AOT_PRODUCTION_FASTPATHS). Run \"Play VC2 (frame census).cmd\", which builds\n"
               "out\\vc2-trace with them.\n";
        std::cerr << "[vc3-census] attribution needs the trace build (\"Play VC2 (frame census).cmd\")\n";
        return;
    }
    if (!final) out << "(still running when written; the game was quit before the hunt finished)\n";
    if (c.events > kMaxAttributionEvents) out << "(stopped early after " << kMaxAttributionEvents << " checks)\n";
    out << "Function 0x00000000 is host code or the kernel (no guest function on the stack).\n"
           "Calls inside one 16 KiB generated unit are not seen: a function's count includes such callees.\n\n";
    std::vector<std::pair<std::uint32_t, const FunctionStats *>> list;
    for (const auto &[pc, stats] : c.functions) list.push_back({pc, &stats});
    std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.second->changes > b.second->changes; });
    for (const auto &[pc, stats] : list) {
        out << hex8(pc) << "  " << std::setw(7) << stats->changes << " changes:";
        for (int k = 0; k < kClasses; ++k)
            if (stats->counts[k] != 0u) out << " " << kClassShort[k] << " " << stats->counts[k];
        out << "\n    words:";
        for (const std::uint32_t a : stats->samples) out << " " << hex8(a);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> callers(stats->callers.begin(), stats->callers.end());
        std::sort(callers.begin(), callers.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        if (!callers.empty()) {
            out << "\n    inside:";
            for (std::size_t i = 0; i < callers.size() && i < 3u; ++i)
                out << " " << hex8(callers[i].first) << " (" << callers[i].second << ")";
        }
        out << "\n";
    }
    if (final)
        std::cerr << "[vc3-census] " << c.attribution_name << " " << number << ": " << c.functions.size()
                  << " functions credited; report " << path.string() << "\n";
}

// --- Swap hunt (F11) ----------------------------------------------------------------
// For a level-of-detail swap and the like: a small number (0, 1, 2 ...) that
// sits still, then steps to a neighbour, then sits still again. For
// VC3_CENSUS_HUNT_SECONDS (default 30) after F11, every word of user RAM is
// watched for such clean steps (between values 0..15, by at most 3, after at
// least 8 frames unchanged); walking back and forth across the swap makes the
// word that drives it step each time. Report hunt-N.txt. Then, for as long
// again (or until each has changed twice), the 64 best are checked around
// every cross-unit call (trace build), naming the code that sets them:
// hunt-N-functions.txt.
//
// Press F12 each time you see the swap happen. A word whose clean step came in
// the second before a mark scores a hit; the report ranks by hits (and puts
// words with steps nowhere near a mark last), so the switch itself comes out
// on top instead of animation states and menus that also step.

constexpr std::size_t kMaxHuntWords = 64u;
constexpr std::uint32_t kNoStep = 0xFFFFFFFFu;
constexpr double kMarkWindowSeconds = 1.0;

void start_hunt(Runtime &rt) {
    Census &c = census();
    // Numbered on from earlier runs' reports, so a new run keeps them.
    std::error_code error;
    do ++c.hunts;
    while (std::filesystem::exists(c.folder / ("hunt-" + std::to_string(c.hunts) + ".txt"), error));
    c.phase = Census::Phase::Hunt;
    c.phase_start = std::chrono::steady_clock::now();
    c.frames_done = 0u;
    read_ram(rt, c.previous);
    c.stable.assign(kWords, 0u);
    c.toggles.assign(kWords, 0u);
    c.changes.assign(kWords, 0u);
    c.last_step.assign(kWords, kNoStep);
    c.hits.assign(kWords, 0u);
    c.last_change.assign(kWords, kNoStep);
    c.any_hits.assign(kWords, 0u);
    c.frame_time.clear();
    c.marks = 0u;
    c.auto_marks = 0u;
    std::cerr << "[vc3-census] swap hunt " << c.hunts << ": for " << c.hunt_seconds
              << " s, cross the point where the model swaps, back and forth; press F12 each time it swaps\n";
}

// A small number stepping to a neighbour: the whole word, or one byte or
// halfword of it with the rest unchanged (a level packed beside other fields).
bool clean_step(std::uint32_t before, std::uint32_t now) {
    const auto small = [](std::uint32_t a, std::uint32_t b) {
        const std::int64_t d = static_cast<std::int64_t>(b) - static_cast<std::int64_t>(a);
        return a <= 15u && b <= 15u && d >= -3 && d <= 3;
    };
    if (small(before, now)) return true;
    const std::uint32_t diff = before ^ now;
    for (unsigned shift = 0; shift < 32u; shift += 8u)
        if ((diff & ~(0xFFu << shift)) == 0u) return small((before >> shift) & 0xFFu, (now >> shift) & 0xFFu);
    for (unsigned shift = 0; shift < 32u; shift += 16u)
        if ((diff & ~(0xFFFFu << shift)) == 0u) return small((before >> shift) & 0xFFFFu, (now >> shift) & 0xFFFFu);
    return false;
}

void hunt_frame(Runtime &rt) {
    Census &c = census();
    read_ram(rt, c.current);
    for (std::uint32_t i = 0; i < kWords; ++i) {
        const std::uint32_t now = c.current[i], before = c.previous[i];
        if (now == before) {
            if (c.stable[i] != 0xFFFFu) ++c.stable[i];
            continue;
        }
        if (c.changes[i] != 0xFFFFu) ++c.changes[i];
        c.last_change[i] = c.frames_done;
        if (c.stable[i] >= 8u && clean_step(before, now)) {
            if (c.toggles[i] != 0xFFu) ++c.toggles[i];
            c.last_step[i] = c.frames_done;
        }
        c.stable[i] = 0u;
    }
    c.previous.swap(c.current);
    c.frame_time.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - c.phase_start).count());
    ++c.frames_done;
}

// F12: the swap just happened. Credit every word that stepped cleanly in the
// second before (each step counts for one mark only).
void hunt_mark() {
    Census &c = census();
    if (c.frame_time.empty()) return;
    ++c.marks;
    const double since = c.frame_time.back() - kMarkWindowSeconds;
    std::uint32_t credited = 0u;
    for (std::uint32_t i = 0; i < kWords; ++i) {
        const std::uint32_t f = c.last_step[i];
        if (f == kNoStep || f >= c.frame_time.size() || c.frame_time[f] < since) continue;
        if (c.hits[i] != 0xFFu) ++c.hits[i];
        c.last_step[i] = kNoStep;
        ++credited;
    }
    std::uint32_t changed = 0u;
    for (std::uint32_t i = 0; i < kWords; ++i) {
        const std::uint32_t f = c.last_change[i];
        if (f == kNoStep || f >= c.frame_time.size() || c.frame_time[f] < since) continue;
        if (c.any_hits[i] != 0xFFu) ++c.any_hits[i];
        c.last_change[i] = kNoStep;
        ++changed;
    }
    std::cerr << "[vc3-census] swap hunt " << c.hunts << ": mark " << c.marks << " (" << credited
              << " words stepped in the second before, " << changed << " changed)\n";
}

void finish_hunt(Runtime &rt) {
    Census &c = census();
    struct Candidate {
        std::uint32_t address, hits, toggles, changes, value;
    };
    std::vector<Candidate> found;
    // With marks: at least half of them hit (and two), and few clean steps
    // away from a mark. Without: stepped cleanly at least twice.
    const std::uint32_t hits_wanted = std::max(2u, (c.marks + 1u) / 2u);
    for (std::uint32_t i = 0; i < kWords; ++i) {
        if (c.toggles[i] < 2u || c.changes[i] > 2u * c.toggles[i] + 1u) continue;
        if (c.marks != 0u && (c.hits[i] < hits_wanted || c.toggles[i] > 2u * c.hits[i] + 2u)) continue;
        found.push_back({kRamStart + i * 4u, c.hits[i], c.toggles[i], c.changes[i], c.previous[i]});
    }
    std::sort(found.begin(), found.end(), [](const Candidate &a, const Candidate &b) {
        if (a.hits != b.hits) return a.hits > b.hits;
        const std::uint32_t a_off = a.toggles - std::min(a.toggles, a.hits), b_off = b.toggles - std::min(b.toggles, b.hits);
        if (a_off != b_off) return a_off < b_off;
        return a.changes < b.changes;
    });
    std::error_code error;
    std::filesystem::create_directories(c.folder, error);
    const std::filesystem::path path = c.folder / ("hunt-" + std::to_string(c.hunts) + ".txt");
    std::ofstream out(path);
    const std::uint32_t static_end = at(kEuStaticEnd) != 0u ? at(kEuStaticEnd) : kEuStaticEnd;
    out << "VC2 swap hunt " << c.hunts << " (" << region_name() << "): " << c.frames_done << " game frames, "
        << c.marks << " F12 marks, " << found.size()
        << (c.marks != 0u ? " words that stepped cleanly just before at least half the marks\n"
                          : " words that stepped cleanly between small values at least twice\n")
        << "(address, steps just before a mark, clean steps, all changes, value now, static = the executable's own "
           "data)\n\n";
    for (const Candidate &f : found)
        out << hex8(f.address) << "  hits " << f.hits << "  steps " << f.toggles << "  changes " << f.changes << "  now " << f.value << "  "
            << (f.address < static_end ? "static" : "heap") << "\n";
    // Any change, not only small steps (a flag of 0x80, a pointer cleared, a
    // float compared with a limit): words that changed in the second before
    // most marks and seldom otherwise.
    std::vector<Candidate> any;
    if (c.marks >= 2u) {
        std::vector<bool> listed(kWords, false);
        for (const Candidate &f : found) listed[(f.address - kRamStart) / 4u] = true;
        for (std::uint32_t i = 0; i < kWords; ++i) {
            if (listed[i] || c.any_hits[i] < hits_wanted || c.changes[i] > 3u * c.any_hits[i] + 2u) continue;
            any.push_back({kRamStart + i * 4u, c.any_hits[i], c.toggles[i], c.changes[i], c.previous[i]});
        }
        std::sort(any.begin(), any.end(), [](const Candidate &a, const Candidate &b) {
            if (a.hits != b.hits) return a.hits > b.hits;
            return a.changes < b.changes;
        });
        out << "\nAny change in the second before at least half the marks, few changes otherwise (" << any.size()
            << "; address, marks hit, clean steps, all changes, value now as hex and float):\n";
        for (std::size_t n = 0; n < any.size() && n < 400u; ++n) {
            const Candidate &f = any[n];
            out << hex8(f.address) << "  hits " << f.hits << "  steps " << f.toggles << "  changes " << f.changes << "  now "
                << hex8(f.value) << " " << std::bit_cast<float>(f.value) << "  "
                << (f.address < static_end ? "static" : "heap") << "\n";
        }
    }
    std::cerr << "[vc3-census] swap hunt " << c.hunts << ": " << found.size() << " candidates, " << any.size()
              << " that change in other ways (" << c.auto_marks << " of " << c.marks << " marks automatic); report "
              << path.string() << "\n";
    std::vector<std::uint16_t>().swap(c.stable);
    std::vector<std::uint8_t>().swap(c.toggles);
    std::vector<std::uint16_t>().swap(c.changes);
    std::vector<std::uint32_t>().swap(c.current);
    std::vector<std::uint32_t>().swap(c.last_step);
    std::vector<std::uint8_t>().swap(c.hits);
    std::vector<std::uint32_t>().swap(c.last_change);
    std::vector<std::uint8_t>().swap(c.any_hits);
    std::vector<double>().swap(c.frame_time);
    // The attribution watches the clean steppers first, then the others.
    for (std::size_t n = 0; n < any.size() && found.size() < kMaxHuntWords; ++n) found.push_back(any[n]);
    if (found.empty()) {
        c.phase = Census::Phase::Idle;
        return;
    }
    c.watch_address.clear();
    c.watch_class.clear();
    for (std::size_t i = 0; i < found.size() && i < kMaxHuntWords; ++i) {
        c.watch_address.push_back(found[i].address);
        c.watch_class.push_back(Other);
    }
    c.watch_value.resize(c.watch_address.size());
    for (std::size_t i = 0; i < c.watch_address.size(); ++i) c.watch_value[i] = rt.memory().aot_load32(c.watch_address[i]);
    c.watch_changes.assign(c.watch_address.size(), 0u);
    c.functions.clear();
    c.stacks.clear();
    c.events = c.chained_events = 0u;
    c.attribution_name = "hunt";
    c.phase = Census::Phase::HuntAttribute;
    c.phase_start = std::chrono::steady_clock::now();
    c.frames_done = 0u;
    c.hooks_on = true;
    psprecomp::set_runtime_pre_dispatch_hook(&on_pre_dispatch);
    psprecomp::set_runtime_post_dispatch_hook(&on_post_dispatch);
    psprecomp::set_runtime_pre_chained_call_hook(&on_pre_call);
    psprecomp::set_runtime_post_chained_call_hook(&on_post_call);
    std::cerr << "[vc3-census] swap hunt " << c.hunts << ": watching the best " << c.watch_address.size()
              << " for up to " << c.hunt_seconds << " s; keep crossing the swap point\n";
}

bool hunt_attribution_done() {
    const Census &c = census();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - c.phase_start).count();
    if (seconds >= c.hunt_seconds || c.events > kMaxAttributionEvents * 50u) return true;
    for (const std::uint32_t n : c.watch_changes)
        if (n < 2u) return false;
    return true;
}

// --- Control hunt (F8) --------------------------------------------------------------
// For the controls overhaul: which words follow the player's input. For
// VC3_CENSUS_HUNT_SECONDS (default 30) after F8, each frame is "active" when
// a PSP button is held or the stick is pushed (from the game's own controller
// reads), "idle" otherwise. Do ONE thing at a time, in bursts: hold a camera
// button for two seconds, let go for two, again (or push the aiming stick in
// Target Mode, and so on). Words that change in most active frames and almost
// never in idle ones are the camera angles, the reticle and the like; words
// equal to the buttons held are the game's copies of the pad. Report
// control-N.txt, then the best 64 are attributed (keep doing the same thing):
// control-N-functions.txt.

struct PadSample {
    std::uint32_t buttons = 0u;
    std::uint8_t lx = 128u, ly = 128u;
    std::uint64_t reads = 0u, peeks = 0u, latches = 0u;
};

PadSample &pad_sample() {
    static PadSample sample;
    return sample;
}

void note_pad(Runtime &rt, std::uint32_t buffer, std::uint32_t count) {
    if (buffer == 0u || count == 0u) return;
    const std::uint32_t last = buffer + (std::min(count, 64u) - 1u) * 16u;
    PadSample &p = pad_sample();
    p.buttons = rt.memory().load32(last + 4u);
    const std::uint32_t analog = rt.memory().load32(last + 8u);
    p.lx = static_cast<std::uint8_t>(analog & 0xFFu);
    p.ly = static_cast<std::uint8_t>((analog >> 8u) & 0xFFu);
}

void start_control(Runtime &rt) {
    Census &c = census();
    std::error_code error;
    do ++c.controls;
    while (std::filesystem::exists(c.folder / ("control-" + std::to_string(c.controls) + ".txt"), error));
    c.phase = Census::Phase::Control;
    c.phase_start = std::chrono::steady_clock::now();
    c.frames_done = 0u;
    read_ram(rt, c.previous);
    c.changed_active.assign(kWords, 0u);
    c.changed_idle.assign(kWords, 0u);
    c.pad_match.assign(kWords, 0u);
    c.active_frames = c.idle_frames = 0u;
    c.held_masks.clear();
    std::cerr << "[vc3-census] control hunt " << c.controls << ": for " << c.hunt_seconds
              << " s, do one thing in bursts (hold for two seconds, let go for two)\n";
}

void control_frame(Runtime &rt) {
    Census &c = census();
    const PadSample &pad = pad_sample();
    // Buttons other than the ones a PSP holds by itself (hold switch, etc.),
    // and other than a modifier held throughout (VC3_CENSUS_CONTROL_IGNORE,
    // e.g. 0x1000: Triangle, held on the academy map to take the camera).
    static const std::uint32_t ignored = [] {
        const char *text = env("CENSUS_CONTROL_IGNORE");
        return text != nullptr ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0)) : 0u;
    }();
    const std::uint32_t buttons = pad.buttons & 0x0000FFFFu & ~ignored;
    const bool stick = std::abs(static_cast<int>(pad.lx) - 128) > 40 || std::abs(static_cast<int>(pad.ly) - 128) > 40;
    const bool active = buttons != 0u || stick;
    read_ram(rt, c.current);
    std::vector<std::uint16_t> &changed = active ? c.changed_active : c.changed_idle;
    for (std::uint32_t i = 0; i < kWords; ++i) {
        if (c.current[i] != c.previous[i] && changed[i] != 0xFFFFu) ++changed[i];
        if (buttons != 0u && (c.current[i] & 0xFFFFu) == buttons && c.pad_match[i] != 0xFFFFu) ++c.pad_match[i];
    }
    c.previous.swap(c.current);
    if (active) {
        ++c.active_frames;
        ++c.held_masks[buttons | (stick ? 0x80000000u : 0u)];
    } else {
        ++c.idle_frames;
    }
    ++c.frames_done;
}

void finish_control(Runtime &rt) {
    Census &c = census();
    struct Candidate {
        std::uint32_t address, active, idle, value;
        double score;
    };
    std::vector<Candidate> followers, pads;
    const double active_frames = std::max(1u, c.active_frames), idle_frames = std::max(1u, c.idle_frames);
    for (std::uint32_t i = 0; i < kWords; ++i) {
        const double a = c.changed_active[i] / active_frames, b = c.changed_idle[i] / idle_frames;
        // A camera that eases keeps moving a little after the stick is let go
        // (the academy map's), so idle changes up to a third of the active
        // rate still count.
        if (c.active_frames >= 10u && a >= 0.3 && b <= a / 3.0)
            followers.push_back({kRamStart + i * 4u, c.changed_active[i], c.changed_idle[i], c.previous[i], a - 2.0 * b});
        if (c.pad_match[i] * 2u >= c.active_frames && c.active_frames >= 10u)
            pads.push_back({kRamStart + i * 4u, c.pad_match[i], 0u, c.previous[i], static_cast<double>(c.pad_match[i])});
    }
    const auto by_score = [](const Candidate &x, const Candidate &y) { return x.score > y.score; };
    std::sort(followers.begin(), followers.end(), by_score);
    std::sort(pads.begin(), pads.end(), by_score);
    std::error_code error;
    std::filesystem::create_directories(c.folder, error);
    const std::filesystem::path path = c.folder / ("control-" + std::to_string(c.controls) + ".txt");
    std::ofstream out(path);
    const std::uint32_t static_end = at(kEuStaticEnd) != 0u ? at(kEuStaticEnd) : kEuStaticEnd;
    out << "VC2 control hunt " << c.controls << " (" << region_name() << "): " << c.frames_done << " game frames, "
        << c.active_frames << " with input, " << c.idle_frames << " without\n"
        << "controller calls: " << pad_sample().reads << " reads, " << pad_sample().peeks << " peeks, "
        << pad_sample().latches << " latch reads so far\n\nInput held (PSP button bits; 80000000 = stick pushed), frames:\n";
    for (const auto &[mask, frames] : c.held_masks) out << "  " << hex8(mask) << "  " << frames << "\n";
    out << "\nWords that follow the input (changed in that share of input frames / of idle frames; value now as hex and "
           "float):\n";
    if (followers.size() > 400u) followers.resize(400u);
    for (const Candidate &f : followers)
        out << hex8(f.address) << "  active " << std::fixed << std::setprecision(2) << f.active / active_frames << "  idle "
            << f.idle / idle_frames << "  now " << hex8(f.value) << " " << std::setprecision(3) << std::bit_cast<float>(f.value)
            << "  " << (f.address < static_end ? "static" : "heap") << "\n";
    out << "\nWords holding the buttons (low 16 bits equal to the buttons held, in that many input frames):\n";
    for (const Candidate &f : pads)
        out << hex8(f.address) << "  " << f.active << "  now " << hex8(f.value) << "  "
            << (f.address < static_end ? "static" : "heap") << "\n";
    std::cerr << "[vc3-census] control hunt " << c.controls << ": " << followers.size() << " followers, " << pads.size()
              << " pad copies; report " << path.string() << "\n";
    std::vector<std::uint16_t>().swap(c.changed_active);
    std::vector<std::uint16_t>().swap(c.changed_idle);
    std::vector<std::uint16_t>().swap(c.pad_match);
    std::vector<std::uint32_t>().swap(c.current);
    if (followers.empty()) {
        c.phase = Census::Phase::Idle;
        return;
    }
    c.watch_address.clear();
    c.watch_class.clear();
    for (std::size_t i = 0; i < followers.size() && i < kMaxHuntWords; ++i) {
        c.watch_address.push_back(followers[i].address);
        c.watch_class.push_back(Other);
    }
    c.watch_value.resize(c.watch_address.size());
    for (std::size_t i = 0; i < c.watch_address.size(); ++i) c.watch_value[i] = rt.memory().aot_load32(c.watch_address[i]);
    c.watch_changes.assign(c.watch_address.size(), 0u);
    c.functions.clear();
    c.stacks.clear();
    c.events = c.chained_events = 0u;
    c.attribution_name = "control";
    c.hunts = c.controls;  // numbers the functions report
    c.phase = Census::Phase::HuntAttribute;
    c.phase_start = std::chrono::steady_clock::now();
    c.frames_done = 0u;
    c.hooks_on = true;
    psprecomp::set_runtime_pre_dispatch_hook(&on_pre_dispatch);
    psprecomp::set_runtime_post_dispatch_hook(&on_post_dispatch);
    psprecomp::set_runtime_pre_chained_call_hook(&on_pre_call);
    psprecomp::set_runtime_post_chained_call_hook(&on_post_call);
    std::cerr << "[vc3-census] control hunt " << c.controls << ": watching the best " << c.watch_address.size()
              << " for up to " << c.hunt_seconds << " s; keep doing the same thing\n";
}

// --- RAM snapshot (F7) -------------------------------------------------------------

void dump_ram(Runtime &rt) {
    Census &c = census();
    std::error_code error;
    std::filesystem::create_directories(c.folder, error);
    static int dumps = 0;
    std::filesystem::path path;
    do path = c.folder / ("ram-" + std::to_string(++dumps) + ".bin");
    while (std::filesystem::exists(path, error));
    std::vector<std::uint32_t> words((0x0A000000u - 0x08800000u) / 4u);
    auto &memory = rt.memory();
    for (std::size_t i = 0; i < words.size(); ++i) words[i] = memory.load32(0x08800000u + static_cast<std::uint32_t>(i) * 4u);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(words.data()), static_cast<std::streamsize>(words.size() * 4u));
    std::cerr << "[vc3-census] RAM 0x08800000-0x0A000000 written to " << path.string() << " (game frame " << c.game_frames
              << ")\n";
}

// --- Frame boundary ----------------------------------------------------------------

void on_frame_start(Runtime &rt) {
    Census &c = census();
    ++c.game_frames;
    const bool down = f10_pressed();
    const bool pressed = down && !c.key_down;
    c.key_down = down;
#if defined(PORTABLEKIT_HAS_SDL)
    const bool hunt_down = key_pressed(SDL_SCANCODE_F11);
    const bool mark_down = key_pressed(SDL_SCANCODE_F12);
#else
    const bool hunt_down = false, mark_down = false;
#endif
    const bool mark_pressed = mark_down && !c.mark_key_down;
    c.mark_key_down = mark_down;
    const bool hunt_pressed = hunt_down && !c.hunt_key_down;
#if defined(PORTABLEKIT_HAS_SDL)
    const bool control_down = key_pressed(SDL_SCANCODE_F8);
#else
    const bool control_down = false;
#endif
    const bool control_pressed = control_down && !c.control_key_down;
#if defined(PORTABLEKIT_HAS_SDL)
    {
        static bool ram_key_down = false;
        const bool ram_down = key_pressed(SDL_SCANCODE_F7);
        if (ram_down && !ram_key_down) dump_ram(rt);
        ram_key_down = ram_down;
    }
#endif
    c.control_key_down = control_down;
    c.hunt_key_down = hunt_down;
    switch (c.phase) {
    case Census::Phase::Idle:
        if (pressed || (c.start_at >= 0 && c.game_frames == static_cast<std::uint64_t>(c.start_at))) {
            ++c.runs;
            c.attribution_name = "census";
            c.watch_changes.clear();
            start_diff(rt);
        } else if (hunt_pressed) {
            start_hunt(rt);
        } else if (control_pressed) {
            start_control(rt);
        }
        break;
    case Census::Phase::Control:
        control_frame(rt);
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - c.phase_start).count() >= c.hunt_seconds)
            finish_control(rt);
        break;
    case Census::Phase::Hunt:
        hunt_frame(rt);
        if (mark_pressed) hunt_mark();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - c.phase_start).count() >= c.hunt_seconds)
            finish_hunt(rt);
        break;
    case Census::Phase::HuntAttribute:
        ++c.frames_done;
        if (c.frames_done % 300u == 0u) write_attribution(false);
        if (hunt_attribution_done()) {
            finish_attribution();
            c.phase = Census::Phase::Idle;
            std::vector<std::uint32_t>().swap(c.previous);
        }
        break;
    case Census::Phase::Diff:
        diff_frame(rt);
        if (c.frames_done >= c.frames_wanted) {
            finish_diff();
            write_census_report();
            if (c.attribute_frames > 0u && !c.words.empty()) start_attribution(rt);
            else c.phase = Census::Phase::Idle;
        }
        break;
    case Census::Phase::Attribute:
        if (++c.frames_done >= c.attribute_frames) {
            finish_attribution();
            c.phase = Census::Phase::Idle;
            std::vector<std::uint32_t>().swap(c.previous);
        }
        break;
    }
}

std::uint32_t parse(const char *name, std::uint32_t fallback, std::uint32_t low, std::uint32_t high) {
    const char *text = env(name);
    if (text == nullptr) return fallback;
    const unsigned long value = std::strtoul(text, nullptr, 0);
    return value >= low && value <= high ? static_cast<std::uint32_t>(value) : fallback;
}

}  // namespace

void census_mark() {
    Census &c = census();
    if (c.phase != Census::Phase::Hunt) return;
    ++c.auto_marks;
    hunt_mark();
}

void register_census(HleRegistrar &hle) {
    const char *folder = env("CENSUS");
    if (folder == nullptr || *folder == '\0') return;
    Census &c = census();
    c.folder = folder;
    c.frames_wanted = parse("CENSUS_FRAMES", 120u, 2u, 3600u);
    c.attribute_frames = parse("CENSUS_ATTRIBUTE_FRAMES", 2u, 0u, 60u);
    c.hunt_seconds = parse("CENSUS_HUNT_SECONDS", 30u, 5u, 300u);
    if (const char *at_text = env("CENSUS_AT")) c.start_at = std::strtoll(at_text, nullptr, 0);
    HleFunction poll = hle.bound_function("ThreadManForUser", kPollSemaNid);
    if (!poll) {
        std::cerr << "[vc3-census] sceKernelPollSema is not bound; census off\n";
        return;
    }
    hle.bind("ThreadManForUser", kPollSemaNid, [inner = std::move(poll)](Runtime &rt, AllegrexContext &ctx) mutable {
        const std::uint32_t sema = arg(ctx, 0u);
        inner(rt, ctx);
        const std::uint32_t block = at(kEuVsyncBlock);
        // The frame start's drain ends when the poll finds the semaphore empty.
        if (block != 0u && sema == rt.memory().load32(block + 24u) && static_cast<std::int32_t>(ctx.gpr[2]) < 0)
            on_frame_start(rt);
    });
    for (const auto &[nid, kind] : {std::pair{0x1F803938u, 0}, std::pair{0x3A622550u, 1}}) {
        HleFunction inner_pad = hle.bound_function("sceCtrl", nid);
        if (!inner_pad) continue;
        hle.bind("sceCtrl", nid, [inner = std::move(inner_pad), kind](Runtime &rt, AllegrexContext &ctx) mutable {
            const std::uint32_t buffer = arg(ctx, 0u), count = std::clamp<std::uint32_t>(arg(ctx, 1u), 1u, 64u);
            inner(rt, ctx);
            note_pad(rt, buffer, count);
            ++(kind == 0 ? pad_sample().reads : pad_sample().peeks);
        });
    }
    std::cerr << "[vc3-census] F8 starts a control hunt; F11 starts a swap hunt (" << c.hunt_seconds << " s; F12 marks each swap); F10 starts a frame census ("
              << c.frames_wanted << " frames, then "
              << c.attribute_frames << " attributed); reports in " << folder << "\n";
}

}  // namespace portablekit::vc3
