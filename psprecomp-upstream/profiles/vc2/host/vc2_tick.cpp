// VC2 at a high frame rate (2026-11-08): VC2_TICK_HZ=<rate> (30..1000) runs
// the game's frame loop at <rate> frames a second, real frames only (PortableKit's
// frame interpolation is kept off). Every release (Japan since 2026-11-15). Report: [vc2-tick]
// every 10 seconds.
//
// Pacing. The PSP's vblank stays at 59.94 Hz: the game hooks several callbacks
// and helper threads onto it (counters, blinking), its vblank interrupt handler
// counts on it, and all of those keep their speed. Only the frame loop is
// paced by our clock: Kurofune's frame start (EU 0x08BB8CDC) waits for the next
// vblank with sceDisplayWaitVblankStartCB (called at 0x08BB8CE8); that one call
// instead waits, in real time, for the next tick of <rate>. Its other wait,
// for the frame interval's further vblanks (0x08BEBBE0, on KfVSyncSema), waits
// for none: the interval is 1. (The first experiment ran the kernel's vblank
// at <rate>, PortableKit patch 0018: per-vblank systems then ran fast too.)
//
// The interval. Menus, story and the battle map ask for a frame every vblank,
// 3D combat every 2 (0x08BD31F4, the frame-rate module's setter, also writes
// the vsync block's interval, EU 0x08D61E40 + 12). It is replaced: it records
// what the game asked for and sets 1.
//
// The step (found with the frame census). VC2 is mostly delta-timed already,
// in 59.94 Hz vblanks: its frame-rate module (EU 0x08D43F98: +0 display rate,
// 59.94; +4 interval; +8 the same as a float) feeds a step = 59.94 / display
// rate x interval (0x08844B04), stored with a scale in its time block (EU
// 0x08D5B918: +28 step = +32 base x +36 scale; setter 0x08844AD4) and handed to
// Kurofune (EU 0x08D61DF8). At least 213 places read it. Other places work it
// out themselves from the module: animated objects advance by speed x interval
// x display rate / a constant of the animation (0x0895AEB0, 0x08A928A4 ...),
// timings divide by the display rate (0x08B6B57C ...). So at <rate> the
// display rate stays 59.94 and the interval becomes the share of a PSP frame
// one of our frames lasts, 59.94 / rate (0.363 at 165): every one of those
// formulas, and the step, then gives the right amount per frame. (Telling the
// module the display ran at <rate> with an interval of 1, the first version,
// made animations 2.75x fast.) Rewritten every vblank; the int interval (+4)
// is 1. VC2_TICK_NO_STEP=1 leaves it all alone.
//
// Whole steps. 26 places truncate a value made from the step to a whole number
// (frame counters add (int)(step + 0.05), others (int)(step x 4) and the like):
// at a fractional step they would add 0 or too little (the Morale meter stalled
// the start of a battle). cmake/vc2_step_sites.cmake turns each into a call to
// vc2_step_trunc(), which keeps a running remainder per site.
//
// Frame-locked additions. Places that add a fixed amount every frame whatever
// the step (the animation player's speed, a unit timer; found with the frame
// census at 165 Hz) become calls to vc2_frame_scale(), which scales the amount
// by step / the interval the game asked for: what the game meant per frame of
// its own rate, spread over ours.

#include "vc2_tick.hpp"
#include "vc2_region.hpp"
#include "vc2_settings.hpp"

#include "profile.hpp"
#include "kernel/kernel.hpp"
#include "psprecomp/corpus_abi.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>

namespace portablekit::vc2 {
namespace {

constexpr std::uint32_t kEuVsyncBlock = 0x08D61E40u;
constexpr std::uint32_t kIntervalOffset = 12u;

constexpr std::uint32_t kEuRateModule = 0x08D43F98u;  // +0 rate (f), +4 interval, +8 interval (f)
constexpr std::uint32_t kEuTimeBlock = 0x08D5B918u;   // +28 step, +32 base, +36 scale (f)
constexpr std::uint32_t kEuEngineStep = 0x08D61DF8u;  // Kurofune's copy of the step (f)
constexpr float kPspRefresh = 59.94f;                 // the game's own constant, 0x426FC28F
constexpr std::uint32_t kEuSetInterval = 0x08BD31F4u;   // frame-rate module: set the frame interval
constexpr std::uint32_t kEuFrameWaitReturn = 0x08BB8CF0u;  // after Kurofune's per-frame vblank wait
constexpr std::uint32_t kWaitVblankStartCbNid = 0x46F186C3u;
// The loading screen ("VlLoading", constructor EU 0x0880EBCC, destructor
// 0x0880ED54): its one instance, 0 while there is none. The game loads a piece
// per frame, so loading took longer at lower frame rates; while this is up the
// frame loop is not paced at all (owner, 2026-11-11: loading as fast as
// possible at every setting). Off unless VC2_LOADING_UNPACED=1: the instance
// is made once and kept (seen in every log since: "up over 60 s"), so it never
// marks a load, and the first minute of every session ran unpaced, at the
// display's rate: 90 FPS on a 165 Hz monitor ran the game 1.8x fast
// (2026-11-14). It needs a signal that is set only while loading.
constexpr std::uint32_t kEuLoadingScreen = 0x08CE9A38u;
// Longer than any load: if the instance stays this long it is not a load (it
// would mean the screen is kept), and frames are paced again.
constexpr double kLongestLoadSeconds = 60.0;

struct TickState {
    // Installed (Europe, US): the interval setter is ours and the frame wait
    // hooked at every rate, so the rate can change while the game runs
    // (in-game Settings > Frame Rate). Active: running at `rate` (or
    // uncapped); inactive: the game's own rate, exactly as without us.
    bool installed = false, active = false;
    bool starting = false;  // install_tick: the game's memory is not set up yet
    settings::FrameRate saved_frame_rate{};  // PortableKit's frame rate at the game's own rate
    double rate = 0.0;
    std::uint32_t interval_address = 0u;
    // The step correction; 0 when off.
    std::uint32_t rate_module = 0u, time_block = 0u, engine_step = 0u;
    float step_base = 1.0f;
    // VC2_TICK_HZ=uncapped: no pacing of our own; the step is measured each
    // frame from the real time since the last one (kept to 1/1000..1/30 s).
    bool uncapped = false;
    double last_frame_us = -1.0;  // emulated time of the last frame start
    // Real (host) time between frame starts, averaged over about 8 frames. When
    // frames come slower than asked (VSync below the asked rate, a heavy scene,
    // or uncapped), the step follows this instead, so the game keeps its speed.
    std::chrono::steady_clock::time_point last_frame_real{};
    double frame_seconds = 0.0;
    std::uint64_t adapted = 0u;  // frames whose step followed the measured time
    float last_game_step = -1.0f;  // the step the game itself last set, for the log
    std::uint64_t step_resets = 0u;
    std::uint64_t vblanks = 0u;
    std::uint64_t held = 0u;        // vblanks at which the game's interval was above 1
    std::uint32_t last_interval = 0u;
    std::chrono::steady_clock::time_point window_start{};
    std::uint64_t window_vblanks = 0u;
    // Pacing.
    std::uint32_t frame_wait_return = 0u;  // 0: not pacing
    double period_us = 0.0;
    double next_frame_us = -1.0;
    float last_written_step = 0.0f;
    std::uint64_t caught_up = 0u;  // frames that waited for emulated time to catch up  // emulated time of the next tick; < 0 before the first
    std::uint64_t frames = 0u, window_frames = 0u, late = 0u;
    // The interval the game asked for (1, or 2 in combat).
    std::uint32_t game_interval = 1u;
};

TickState &tick() {
    static TickState state;
    return state;
}

// Whether the loading screen is up (Europe), with a log line as it comes and
// goes: how long it was up and how many frames ran.
struct LoadingState {
    bool enabled = false;
    std::uint32_t address = 0u;
    bool up = false;
    bool too_long = false;
    std::chrono::steady_clock::time_point since{};
    std::uint64_t frames = 0u;
};
LoadingState &loading_state() {
    static LoadingState state = [] {
        LoadingState s;
        s.enabled = env("LOADING_UNPACED") != nullptr;
        s.address = s.enabled ? at(kEuLoadingScreen) : 0u;
        s.enabled = s.address != 0u;
        return s;
    }();
    return state;
}

// Called once a frame, at the frame start.
bool loading_now() {
    LoadingState &l = loading_state();
    if (!l.enabled) return false;
    const bool screen = kernel().runtime().memory().load32(l.address) != 0u;
    const auto now = std::chrono::steady_clock::now();
    if (screen && !l.up) {
        l.up = true;
        l.too_long = false;
        l.since = now;
        l.frames = 0u;
    } else if (!screen && l.up) {
        l.up = false;
        std::cerr << "[vc2-tick] loading screen: " << std::fixed << std::setprecision(2)
                  << std::chrono::duration<double>(now - l.since).count() << " s, " << l.frames
                  << " frames, unpaced" << (l.too_long ? " (paced again after 60 s)" : "") << "\n";
    }
    if (!l.up) return false;
    ++l.frames;
    if (!l.too_long && std::chrono::duration<double>(now - l.since).count() > kLongestLoadSeconds) {
        l.too_long = true;
        std::cerr << "[vc2-tick] the loading screen has been up over 60 s: frames paced again\n";
    }
    return !l.too_long;
}

// vc2_step_trunc's state: on while the step is corrected.
bool g_step_sites_active = false;
// While a frame census runs: the step alternates frame by frame (x1, x1.5), so
// what follows it changes by alternating amounts and only what ignores it
// changes steadily (set_step_wobble).
bool g_step_wobble = false;
struct StepSite {
    std::uint32_t pc;
    double bias;        // a constant the game adds so 1.0 does not truncate to 0.9999's 0
    double remainder = 0.0;
    std::uint64_t calls = 0u;
};
// The sites cmake/vc2_step_sites.cmake patches; the two frame counters that
// truncate (step + 0.05) leave the 0.05 out of the running total.
std::array<StepSite, 26> g_step_sites{{
    {0x08813600u, 0.05}, {0x08886958u, 0.0}, {0x0888B8B0u, 0.0}, {0x0888F500u, 0.0}, {0x0889DEF8u, 0.0},
    {0x088ACEF0u, 0.05}, {0x088AECE4u, 0.0}, {0x088BCC0Cu, 0.0}, {0x0890D84Cu, 0.0}, {0x0891B8ECu, 0.0},
    {0x0892E6ACu, 0.0},  {0x0893F04Cu, 0.0}, {0x08941778u, 0.0}, {0x0896EC90u, 0.0}, {0x0896ECB4u, 0.0},
    {0x0897A03Cu, 0.0},  {0x0897ACE0u, 0.0}, {0x0897C2C0u, 0.0}, {0x089A2468u, 0.0}, {0x089AC8DCu, 0.0},
    {0x089ACB04u, 0.0},  {0x089ACB40u, 0.0}, {0x08A54438u, 0.0}, {0x08B09A9Cu, 0.0}, {0x08B60068u, 0.0},
    {0x08B60164u, 0.0},
}};

// The frame-locked sites (cmake/vc2_step_sites.cmake): calls, for the report,
// and the integer counters' running remainders.
struct FrameSite {
    std::uint32_t pc;
    const char *what;
    double remainder = 0.0;
    std::uint64_t calls = 0u;
    bool skipped = false;  // VC2_TICK_SKIP_SITES: left as the game wrote it
};
std::array<FrameSite, 8> g_frame_sites{{
    {0x08893C48u, "unit timer"},
    // The animation player's three sites are counted but no longer scaled:
    // since the frame-rate module fix its controller speed is already per
    // step, so scaling again slowed it by the step a second time (the academy
    // map's water vanished for ~24 s of every ~41 at 165 Hz; fixed by leaving
    // them alone, user-confirmed 2026-11-08 19:13).
    {0x08C5953Cu, "animation player (track)", 0.0, 0u, true},
    {0x08C59550u, "animation player (track)", 0.0, 0u, true},
    {0x08C59584u, "animation player", 0.0, 0u, true},
    {0x088BFCC8u, "unit counter +2096"},
    {0x08BD2EACu, "main loop counter"},
    {0x08A4C170u, "academy map cursor x"},
    {0x08A4C178u, "academy map cursor y"},
}};
// VC2_TICK_SKIP_SITES=<pc>,<pc>,...: more frame-locked sites to leave
// unscaled, to test whether one is still needed.
void read_skipped_sites() {
    static bool done = false;
    if (done) return;
    done = true;
    const char *text = env("TICK_SKIP_SITES");
    if (text == nullptr) return;
    for (const char *p = text; *p != '\0';) {
        char *end = nullptr;
        const unsigned long pc = std::strtoul(p, &end, 16);
        if (end == p) {
            ++p;
            continue;
        }
        for (FrameSite &site : g_frame_sites)
            if (site.pc == pc) {
                site.skipped = true;
                std::cerr << "[vc2-tick] frame-locked site 0x" << std::hex << site.pc << std::dec << " (" << site.what
                          << ") left unscaled\n";
            }
        p = end;
    }
}

FrameSite *frame_site(std::uint32_t pc) {
    read_skipped_sites();
    for (FrameSite &site : g_frame_sites)
        if (site.pc == pc) return &site;
    return nullptr;
}

// trunc.w.s as the PSP does it (and fpu_float_to_word_ct<1u>).
std::uint32_t psp_trunc(double value) {
    if (std::isnan(value)) return 0x7FFFFFFFu;
    if (value <= -2147483648.0) return 0x80000000u;
    if (value >= 2147483647.0) return 0x7FFFFFFFu;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

// The frame-rate module and the step, from step_base (x wobble for the census):
// display rate 59.94, interval = the share of a PSP frame one of our frames
// lasts, step = that x the time block's scale.
void write_step(float wobble) {
    TickState &t = tick();
    auto &memory = kernel().runtime().memory();
    const auto loadf = [&](std::uint32_t a) { return std::bit_cast<float>(memory.load32(a)); };
    const auto storef = [&](std::uint32_t a, float v) { memory.store32(a, std::bit_cast<std::uint32_t>(v)); };
    const float scale = loadf(t.time_block + 36u);
    const float base = t.step_base * wobble;
    const float step = base * scale;
    const float current = loadf(t.time_block + 28u);
    // The game sets its step when the mode changes (1 in menus, 2 in combat).
    if (wobble == 1.0f && current != step && current != t.last_written_step) {
        if (current != t.last_game_step)
            std::cerr << "[vc2-tick] the game set its step to " << current << " (interval "
                      << memory.load32(t.rate_module + 4u) << ", scale " << scale << "); now " << step << "\n";
        t.last_game_step = current;
        ++t.step_resets;
    }
    storef(t.rate_module + 0u, kPspRefresh);
    memory.store32(t.rate_module + 4u, 1u);
    storef(t.rate_module + 8u, base);
    storef(t.time_block + 32u, base);
    storef(t.time_block + 28u, step);
    storef(t.engine_step, step);
    t.last_written_step = step;
}

void on_tick() {
    TickState &t = tick();
    if (!t.active) return;
    ++t.vblanks;
    ++t.window_vblanks;
    if (t.interval_address != 0u) {
        auto &memory = kernel().runtime().memory();
        const std::uint32_t interval = memory.load32(t.interval_address);
        if (interval != t.last_interval) {
            std::cerr << "[vc2-tick] the game asks for a frame every " << interval << " vblank(s)"
                      << (interval > 1u && interval < 16u ? "; held at 1" : "") << "\n";
            t.last_interval = interval;
        }
        if (interval > 1u && interval < 16u) {
            memory.store32(t.interval_address, 1u);
            t.last_interval = 1u;
            ++t.held;
        }
    }
    // The step is kept by the frame start when it changes frame by frame
    // (uncapped, or the census's wobble).
    if (t.time_block != 0u && !g_step_wobble) write_step(1.0f);
    const auto now = std::chrono::steady_clock::now();
    if (t.window_start == std::chrono::steady_clock::time_point{}) t.window_start = now;
    const double seconds = std::chrono::duration<double>(now - t.window_start).count();
    if (seconds >= 10.0) {
        std::cerr << "[vc2-tick] " << std::fixed << std::setprecision(1) << t.window_frames / seconds
                  << " frames a second (asked " << (t.uncapped ? std::string("uncapped") : std::to_string(t.rate)) << ", "
                  << t.late << " late, " << t.adapted << " with a measured step, " << t.caught_up
                  << " waited for emulated time so far), "
                  << t.window_vblanks / seconds << " vblanks; game interval " << t.game_interval
                  << "; interval held at 1 " << t.held
                  << " times so far; step " << t.step_base << (t.time_block != 0u ? "" : " (not corrected)")
                  << ", reset by the game " << t.step_resets << " times";
        if (g_step_sites_active) {
            int used = 0;
            for (const auto &site : g_step_sites) used += site.calls != 0u ? 1 : 0;
            std::cerr << "; " << used << " of " << g_step_sites.size() << " whole-step sites reached; frame-locked sites:";
            for (const FrameSite &site : g_frame_sites) std::cerr << " " << site.what << " " << site.calls;
        }
        std::cerr << "\n";
        t.window_start = now;
        t.window_vblanks = 0u;
        t.window_frames = 0u;
    }
}

// The frame-rate module's interval setter (EU 0x08BD31F4, a0 = vblanks per
// frame): as the game's, with 1 for whatever it asks; what it asked for is kept
// for vc2_frame_scale.
void set_interval(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    TickState &t = tick();
    auto &memory = static_cast<Runtime &>(corpus).memory();
    const std::uint32_t asked = ctx.gpr[4];
    if (!t.active) {
        // The game's own rate: what the game's function does (EU 0x08BD31F4):
        // the vsync block's interval (0x08BEBC9C), then the module's interval
        // as an int (+4) and a float (+8).
        t.game_interval = asked >= 1u && asked <= 8u ? asked : 1u;
        memory.store32(at(kEuVsyncBlock + kIntervalOffset), asked);
        const std::uint32_t module = at(kEuRateModule);
        memory.store32(module + 4u, asked);
        memory.store32(module + 8u, std::bit_cast<std::uint32_t>(static_cast<float>(static_cast<std::int32_t>(asked))));
        ctx.pc = ctx.gpr[31];
        return;
    }
    if (asked != t.game_interval)
        std::cerr << "[vc2-tick] the game asks for a frame every " << asked << " vblank(s); 1 at " << t.rate << " Hz\n";
    t.game_interval = asked >= 1u && asked <= 8u ? asked : 1u;
    memory.store32(at(kEuVsyncBlock + kIntervalOffset), 1u);
    if (t.rate_module != 0u) {
        memory.store32(t.rate_module + 4u, 1u);
        memory.store32(t.rate_module + 8u, std::bit_cast<std::uint32_t>(t.step_base));
    }
    ctx.pc = ctx.gpr[31];
}

// Kurofune's per-frame vblank wait, from the frame start: the next tick of our
// clock instead, as an ordinary timed delay in emulated time, which the kernel
// holds to real time. (A host wait polled in real time stalled the emulated
// clock: everything ran at 2%.) A frame that comes more than a tick late starts
// the clock again from now rather than hurrying the ones after it.
void measure_frame() {
    TickState &t = tick();
    using Clock = std::chrono::steady_clock;
    const Clock::time_point now = Clock::now();
    const double asked = t.uncapped ? 0.0 : t.period_us / 1e6;
    if (t.last_frame_real != Clock::time_point{}) {
        // One slow frame (a load, a hitch) counts as at most a 30th of a
        // second: beyond that the game slows down, as on the PSP.
        const double sample = std::clamp(std::chrono::duration<double>(now - t.last_frame_real).count(),
                                         1.0 / 2000.0, 1.0 / 30.0);
        t.frame_seconds = t.frame_seconds <= 0.0 ? sample : t.frame_seconds + (sample - t.frame_seconds) / 8.0;
    }
    t.last_frame_real = now;
    if (t.time_block == 0u || t.frame_seconds <= 0.0) return;
    // On time (within 4%): exactly the asked step, so motion is even.
    const double seconds = t.frame_seconds > asked * 1.04 ? t.frame_seconds : asked;
    if (seconds != asked) ++t.adapted;
    t.step_base = static_cast<float>(seconds * kPspRefresh);
}

void wait_for_tick(AllegrexContext &ctx) {
    TickState &t = tick();
    const double now = static_cast<double>(kernel().now_us());
    ++t.frames;
    ++t.window_frames;
    measure_frame();
    if (t.time_block != 0u) write_step(g_step_wobble && (t.frames & 1u) != 0u ? 1.5f : 1.0f);
    // Uncapped runs the same clock at 1000 Hz (a catch-up wait of its own fed
    // back into the frame time: it held uncapped at 72-97 FPS, 2026-11-08), so
    // frames come as fast as the PC allows and the step follows the measured
    // frame time.
    double due = now;
    if (loading_now()) {
        // Loading: no pacing; the clock starts again after it.
        t.next_frame_us = -1.0;
    } else {
        if (t.next_frame_us < 0.0 || now > t.next_frame_us + t.period_us) {
            if (t.next_frame_us >= 0.0) ++t.late;
            t.next_frame_us = now;
        }
        due = t.next_frame_us;
        t.next_frame_us += t.period_us;
    }
    // When frames come back to back (uncapped, or a rate the PC only just
    // makes), nothing waits, and emulated time (charged only for the game's own
    // running) falls behind real time: the game's timers and vblanks ran at
    // 65% in 3D at 640 FPS uncapped. Wait out the lag the kernel itself sees,
    // which costs no real time (it is behind, so the kernel does not sleep).
    if (const auto mapped = kernel().real_time_of(kernel().now_us())) {
        const double lag_us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - *mapped).count();
        if (lag_us > 1000.0 && lag_us < 100000.0) {
            due = std::max(due, now + lag_us);
            ++t.caught_up;
        }
    }
    if (now >= due) {
        kernel().finish(ctx, 0u);
        return;
    }
    kernel().delay_current(ctx, static_cast<std::uint64_t>(std::ceil(due - now)), 0u);
}

}  // namespace

void set_step_wobble(bool on) { g_step_wobble = on && g_step_sites_active; }
bool step_wobble() { return g_step_wobble; }

float corrected_step() { return g_step_sites_active ? tick().step_base : 0.0f; }

// The rate now: "0" (the game's own), "uncapped" or 30..1000 frames a second.
bool set_frame_rate(const std::string &value) {
    TickState &t = tick();
    if (!t.installed) return false;
    auto &memory = kernel().runtime().memory();
    if (value == "0") {
        if (!t.active) return true;
        t.active = false;
        g_step_sites_active = false;
        g_step_wobble = false;
        // Back to the game's own interval and step (what 0x08844B04 works out
        // at 59.94 Hz: step = interval x scale).
        const std::uint32_t interval = t.game_interval;
        const float base = static_cast<float>(interval);
        memory.store32(at(kEuVsyncBlock + kIntervalOffset), interval);
        const std::uint32_t module = at(kEuRateModule);
        memory.store32(module + 0u, std::bit_cast<std::uint32_t>(kPspRefresh));
        memory.store32(module + 4u, interval);
        memory.store32(module + 8u, std::bit_cast<std::uint32_t>(base));
        if (t.time_block != 0u) {
            const float scale = std::bit_cast<float>(memory.load32(t.time_block + 36u));
            memory.store32(t.time_block + 32u, std::bit_cast<std::uint32_t>(base));
            memory.store32(t.time_block + 28u, std::bit_cast<std::uint32_t>(base * scale));
            memory.store32(t.engine_step, std::bit_cast<std::uint32_t>(base * scale));
        }
        settings::current().frame_rate = t.saved_frame_rate;
        std::cerr << "[vc2-tick] the game's own frame rate (interval " << interval << ")\n";
        return true;
    }
    const bool uncapped = value == "uncapped";
    const double rate = uncapped ? 1000.0 : std::strtod(value.c_str(), nullptr);  // uncapped: a 1000 Hz ceiling
    if (!(rate >= 30.0 && rate <= 1000.0)) {
        std::cerr << "[vc2-tick] frame rate " << value << " ignored (30 to 1000, or uncapped)\n";
        return false;
    }
    if (!t.active) t.saved_frame_rate = settings::current().frame_rate;
    t.rate = rate;
    t.uncapped = uncapped;
    t.period_us = 1e6 / rate;
    t.next_frame_us = -1.0;
    t.frame_seconds = 0.0;
    t.last_frame_real = {};
    // Real frames only: no interpolated ones in between.
    settings::current().frame_rate = settings::FrameRate::Fps30;
    if (t.time_block != 0u) {
        t.step_base = kPspRefresh / static_cast<float>(rate);
        g_step_sites_active = true;
    }
    t.active = true;
    if (t.starting) {  // the vblank hook writes them from the first vblank on
        std::cerr << "[vc2-tick] the game's frame loop runs "
                  << (uncapped ? std::string("uncapped") : "at " + std::to_string(rate) + " Hz")
                  << " (the PSP's vblank stays at 59.94); step " << t.step_base << " of a PSP frame\n";
        return true;
    }
    memory.store32(t.interval_address, 1u);
    if (t.time_block != 0u) {
        memory.store32(t.rate_module + 4u, 1u);
        write_step(1.0f);
    }
    std::cerr << "[vc2-tick] the game's frame loop runs "
              << (uncapped ? std::string("uncapped, the step measured each frame") : "at " + std::to_string(rate) + " Hz")
              << " (the PSP's vblank stays at 59.94)"
              << (t.time_block != 0u ? ". The game's step is " : ". The game's step is not corrected: it runs faster")
              << (t.time_block != 0u ? std::to_string(t.step_base) + " of a PSP frame." : std::string()) << "\n";
    return true;
}

bool frame_rate_live() { return tick().installed; }

void install_tick(Runtime &rt) {
    const std::string &text = options::get(options::Key::TickHz);
    if (!code_matches(rt, kEuSetInterval)) {
        std::cerr << "[vc2-tick] the frame-rate module is not where expected; frame rate stays the game's\n";
        return;
    }
    TickState &t = tick();
    t.installed = true;
    t.saved_frame_rate = settings::current().frame_rate;
    t.frame_wait_return = at(kEuFrameWaitReturn);
    t.interval_address = at(kEuVsyncBlock + kIntervalOffset);
    rt.register_function(at(kEuSetInterval), &set_interval, "vc2_set_frame_interval");
    if (env("TICK_NO_STEP") == nullptr) {
        t.rate_module = at(kEuRateModule);
        t.time_block = at(kEuTimeBlock);
        t.engine_step = at(kEuEngineStep);
        if (t.rate_module == 0u || t.time_block == 0u || t.engine_step == 0u) t.time_block = 0u;
    }
    kernel().add_vblank_hook(&on_tick);
    t.starting = true;
    if (text != "0") (void)set_frame_rate(text);
    t.starting = false;
}

void register_tick(HleRegistrar &hle) {
    // Bound at every frame rate: at the game's own rate too, the loading screen
    // runs unpaced (no wait for the vblank at the frame start).
    HleFunction inner = hle.bound_function("sceDisplay", kWaitVblankStartCbNid);
    if (!inner) {
        std::cerr << "[vc2-tick] sceDisplayWaitVblankStartCB is not bound; frames stay on the vblank\n";
        return;
    }
    if (loading_state().enabled)
        std::cerr << "[vc2-tick] loading screens run unpaced (VC2_LOADING_UNPACED=1)\n";
    hle.bind("sceDisplay", kWaitVblankStartCbNid, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
        const TickState &t = tick();
        if (t.active && ctx.gpr[31] == t.frame_wait_return) {
            wait_for_tick(ctx);
            return;
        }
        // The game's own rate: the frame start's vblank wait is skipped while
        // loading.
        if (!t.active && ctx.gpr[31] == at(kEuFrameWaitReturn) && loading_now()) {
            kernel().finish(ctx, 0u);
            return;
        }
        inner(rt, ctx);
    });
}

}  // namespace portablekit::vc2

// Called by the patched generated code (cmake/vc2_step_sites.cmake) in place
// of trunc.w.s at each site.
extern "C" std::uint32_t vc2_step_trunc(std::uint32_t pc, float value) noexcept {
    using namespace portablekit::vc2;
    if (!g_step_sites_active || !std::isfinite(value)) return psp_trunc(value);
    // The battle HUD's morale gauge (EU 0x0896EC90 / 0x0896ECB4: its move is
    // trunc(step x 5), or x 4) is updated on the PSP's own vblank, about 60
    // times a second at any frame rate, not once a game frame, so the step
    // (one of our frames) made it crawl at high rates. It takes the step the
    // game itself chose (its interval: 1, or 2 in battle), and moves as at the
    // game's rate at any rate. (Found in VC3, 2026-11-16: 58-60 updates a
    // second at 240 Hz moving a quarter as far; confirmed there by the owner.)
    if (pc == 0x0896EC90u || pc == 0x0896ECB4u) {
        const TickState &t = tick();
        const float step = t.time_block != 0u ? std::bit_cast<float>(portablekit::kernel().runtime().memory().load32(t.time_block + 28u))
                                              : 0.0f;
        if (step > 1e-4f) value = value / step * static_cast<float>(t.game_interval);
    }
    for (StepSite &site : g_step_sites) {
        if (site.pc != pc) continue;
        ++site.calls;
        const double total = site.remainder + static_cast<double>(value) - site.bias;
        // Whole steps so far; 1e-6 so 0.363 x 165 frames lands on 60, not 59.
        const double whole = total >= 0.0 ? std::floor(total + 1e-6) : std::ceil(total - 1e-6);
        site.remainder = total - whole;
        return psp_trunc(whole);
    }
    return psp_trunc(value);
}

// The battle HUD's morale gauge (cmake/vc2_step_sites.cmake, after step sites
// 0x0896EC90 / 0x0896ECB4): `amount` is this frame's move, `position` the
// gauge (0 = empty), `morale` its target / 10. A moving gauge at exactly 0
// plays a morale drop, so a rise from 0 must not stand still for a frame,
// which the whole-step amount does above 165 Hz (step x 4 < 1).
extern "C" std::uint32_t vc2_morale_step(std::uint32_t amount, std::uint32_t position, std::uint32_t morale) noexcept {
    using namespace portablekit::vc2;
    static bool told = false;
    if (amount == 0u && position == 0u && static_cast<std::int32_t>(morale) > 0) {
        if (!told) {
            told = true;
            std::cerr << "[vc2-tick] morale gauge: first move from empty made 1 (no false morale drop)\n";
        }
        return 1u;
    }
    return amount;
}

// Called by the patched generated code (cmake/vc2_step_sites.cmake) in place
// of a frame-locked `x += value`: the value per frame of the game's own rate,
// spread over ours.
extern "C" float vc2_frame_scale(std::uint32_t pc, float value) noexcept {
    using namespace portablekit::vc2;
    if (!g_step_sites_active) return value;
    if (FrameSite *site = frame_site(pc)) {
        ++site->calls;
        if (site->skipped) return value;
    }
    const TickState &t = tick();
    // Meant per (interval / 59.94) s; a frame of ours is (step_base / 59.94) s:
    // value x step_base / interval (x 0.363 at 165 Hz, x 0.18 in combat).
    return value * t.step_base / static_cast<float>(t.game_interval);
}

// In place of a frame-locked `n += amount` (whole numbers): the same scale as
// vc2_frame_scale, with a running remainder per site, so the counter gains the
// whole part or one more each frame and the right amount over time.
extern "C" std::uint32_t vc2_frame_count(std::uint32_t pc, std::uint32_t amount) noexcept {
    using namespace portablekit::vc2;
    if (!g_step_sites_active) return amount;
    FrameSite *site = frame_site(pc);
    if (site == nullptr) return amount;
    ++site->calls;
    if (site->skipped) return amount;
    const TickState &t = tick();
    const double total = site->remainder + static_cast<double>(static_cast<std::int32_t>(amount)) *
                                               static_cast<double>(t.step_base) / static_cast<double>(t.game_interval);
    const double whole = total >= 0.0 ? std::floor(total + 1e-6) : std::ceil(total - 1e-6);
    site->remainder = total - whole;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(whole));
}
