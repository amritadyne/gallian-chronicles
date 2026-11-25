// Values specific to Valkyria Chronicles II, European release ULES01417.
// This profile contains no game code or assets.  The executable and disc image
// are supplied locally by the player under profiles/vc2/game/.

#include "profile.hpp"
#include "hle/hle_common.hpp"
#include "vc2_font.hpp"
#include "vc2_tick.hpp"
#include "vc2_camera.hpp"
#include "vc2_controls.hpp"
#include "vc2_discord.hpp"
#include "vc2_audio.hpp"
#include "vc2_online.hpp"
#include "vc2_glyphs.hpp"
#include "vc2_census.hpp"
#include "vc2_native_gu.hpp"
#include "vc2_region.hpp"
#include "vc2_settings.hpp"
#include "vc2_text.hpp"
#include "vc2_cheats.hpp"
#include "vc2_extras.hpp"
#include "vc2_voice.hpp"
#include "vc2_music_reader.hpp"
#include "vc2_soundtrack_table.hpp"
#include "adhoc/session.hpp"
#include "gpu/ge_state.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#if defined(PORTABLEKIT_HAS_SDL)
#include <SDL3/SDL.h>
#endif
#if defined(PORTABLEKIT_HAS_RENDERER)
#include "ui/ui.hpp"
#endif

namespace portablekit {
namespace {

constexpr SaveFolder kSaveFolders[] = {
    {"ULES01417DATA", "Valkyria Chronicles II save data", true},
    {"ULUS10515DATA", "Valkyria Chronicles II save data (US release)", true},
    {"NPJH50145DATA", "Senjou no Valkyria 2 save data (Japanese release)", true},
};

constexpr std::uint32_t kFontStyleBytes = 168u;

// --- CRI middleware diagnostic (VC2_TRACE_CRI) -------------------------------
// Static analysis of the supported EBOOT.ELF (load base 0x08804000) located
// CRI's error notifier at ELF 0x437858.  It counts every error in globals
// relocated against segment 1 (ELF 0x52d548) and forwards it to a callback
// only if the game registered one, so CRI file-system/CPK failures can be
// silent.  The same library contains CRI's UTF-table XOR decryptor at ELF
// 0x436f58, selected by byte 4 of a CPK header, so an encrypted ODIN.CPK
// table is expected and not by itself a fault.
//
// Same-unit guest calls are C++ gotos in the AOT corpus, so a function-entry
// hook cannot observe these routines.  Instead, when VC2_TRACE_CRI is set,
// the per-vblank sceDisplaySetFrameBuf wrapper installs an unused import
// stub as CRI's error callback (only while the slot is still null) and reports
// counter changes plus the startup CPK buffer the progress log identified.
// The callback stub forwards to the original import for any other caller.
// Diagnostic only: it changes guest state solely when the variable is set.
constexpr std::uint32_t kEuCriErrCallback = 0x08D565A4u;    // ELF seg1 + 0x2505c
constexpr std::uint32_t kEuCriErrSuppress = 0x08D565A8u;    // + 0x25060
constexpr std::uint32_t kEuCriErrCount = 0x08D565ACu;       // + 0x25064
constexpr std::uint32_t kEuCriErrSuppressed = 0x08D565B0u;  // + 0x25068
constexpr std::uint32_t kEuCriErrFormatted = 0x08D565B4u;   // + 0x2506c, 256 bytes
constexpr std::uint32_t kEuCriNotifyReturn = 0x08C3B910u;   // ELF 0x437910, after jalr
constexpr std::uint32_t kEuCpkStartupBuffer = 0x091065C0u;  // progress log 2026-11-06 #20
// sceNetAdhocMatching NID 0xF16EAF4F: stub ELF 0x4acec0. Ad hoc matching is
// not reached during boot; any other caller is forwarded unchanged.
constexpr std::uint32_t kEuCriCallbackStub = 0x08CB0EC0u;
constexpr std::uint32_t kCriCallbackNid = 0xF16EAF4Fu;

struct CriTraceState {
    bool announced_install = false;
    bool announced_foreign = false;
    std::uint32_t last_count = 0xFFFFFFFFu;
    std::uint32_t last_suppressed = 0xFFFFFFFFu;
    std::array<std::uint8_t, 32> last_cpk{};
    bool have_cpk = false;
    std::uint64_t frames = 0u;
};

CriTraceState &cri_trace() {
    static CriTraceState state;
    return state;
}

std::string hex_bytes(const std::uint8_t *bytes, std::size_t size) {
    std::string text;
    char buf[4];
    for (std::size_t i = 0; i < size; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", bytes[i]);
        text += buf;
        if ((i & 3u) == 3u && i + 1u < size) text += ' ';
    }
    text += " |";
    for (std::size_t i = 0; i < size; ++i)
        text += (bytes[i] >= 0x20u && bytes[i] < 0x7Fu) ? static_cast<char>(bytes[i]) : '.';
    return text + "|";
}

// Heuristic backtrace for the per-frame caller: $ra plus stack words that
// point into the main executable's .text (ELF 0..0x4ac9e0).
void sample_frame_caller(Runtime &rt, const AllegrexContext &ctx, std::uint64_t frame) {
    constexpr std::uint32_t kTextStart = 0x08804000u;
    const std::uint32_t kTextEnd = vc2::at(0x08804000u + 0x4ac9e0u);  // .text end of the running edition
    auto &memory = rt.memory();
    std::cerr << "[vc2-frame] frame " << frame << " ra=0x" << std::hex << ctx.gpr[31]
              << " (elf 0x" << (ctx.gpr[31] - kTextStart) << ") sp=0x" << ctx.gpr[29] << " stack-code:";
    const std::uint32_t sp = ctx.gpr[29];
    unsigned shown = 0u;
    for (std::uint32_t offset = 0u; offset < 0x400u && shown < 16u; offset += 4u) {
        if (!memory.contains(sp + offset, 4u)) break;
        const std::uint32_t word = memory.load32(sp + offset);
        if (word >= kTextStart && word < kTextEnd && (word & 3u) == 0u) {
            std::cerr << " +" << offset << ":" << (word - kTextStart);
            ++shown;
        }
    }
    std::cerr << std::dec << "\n";
}

// Full HLE histogram at frame 300 and 600 plus the difference: what the
// steady-state loop calls every frame versus what ran only during startup.
std::map<std::string, std::uint64_t> &histogram_snapshot() {
    static std::map<std::string, std::uint64_t> snapshot;
    return snapshot;
}

void report_histogram(Runtime &rt, std::uint64_t frame) {
    const auto entries = rt.hle_histogram();
    auto &previous = histogram_snapshot();
    if (frame == 600u) {
        std::cerr << "[vc2-hist] frame 600 full histogram (" << entries.size() << " imports), delta since frame 300:\n";
        for (const auto &[name, count] : entries) {
            const auto before = previous.find(name);
            const std::uint64_t delta = before == previous.end() ? count : count - before->second;
            std::cerr << "[vc2-hist] " << name << " total=" << count << " last300=" << delta << "\n";
        }
    }
    previous.clear();
    for (const auto &[name, count] : entries) previous[name] = count;
}

// The double-buffered 256 KiB frame list buffers VC2 enqueues each frame.
// Their first two words are SIGNAL(2)+END; dump what follows, count GE
// commands by kind, and locate the terminating FINISH+END.
void dump_frame_list(Runtime &rt) {
    auto &memory = rt.memory();
    for (const std::uint32_t base : {0x09BDF500u, 0x09C1F4C0u}) {
        if (!memory.contains(base, 0x3FFC0u)) continue;
        std::cerr << "[vc2-list] buffer 0x" << std::hex << base << " first words:";
        for (std::uint32_t i = 0; i < 40u; ++i) std::cerr << (i % 8u == 0u ? "\n[vc2-list]   " : " ") << std::setw(8) << std::setfill('0') << memory.load32(base + i * 4u);
        std::cerr << std::setfill(' ') << "\n";
        std::array<std::uint32_t, 256> kinds{};
        std::uint32_t finish_at = 0u, words = 0u;
        for (std::uint32_t off = 8u; off + 4u < 0x3FFC0u; off += 4u) {
            const std::uint32_t word = memory.load32(base + off);
            ++kinds[word >> 24u];
            ++words;
            if ((word >> 24u) == 0x0Fu && (memory.load32(base + off + 4u) >> 24u) == 0x0Cu) {
                finish_at = off;
                break;
            }
        }
        std::cerr << "[vc2-list] buffer 0x" << base << ": FINISH+END at +0x" << finish_at << std::dec << " after "
                  << words << " words; PRIM=" << kinds[0x04] << " BEZIER=" << kinds[0x05] << " SPLINE=" << kinds[0x06]
                  << " CALL=" << kinds[0x0A] << " JUMP=" << kinds[0x08] << " RET=" << kinds[0x0B] << " SIGNAL=" << kinds[0x0E]
                  << " END=" << kinds[0x0C] << " TEX0=" << kinds[0xA0] << " XFER=" << kinds[0xEA] << "\n";
    }
}

bool &poll_window() {
    static bool active = false;
    return active;
}

// Census of chained (jalr / cross-unit) and outer-dispatch targets during
// frames 600-601: which game tasks and virtual handlers actually run.
std::map<std::uint32_t, std::uint64_t> &call_census() {
    static std::map<std::uint32_t, std::uint64_t> census;
    return census;
}

void census_chained(psprecomp::Runtime &, psprecomp::AllegrexContext &, std::uint32_t target, std::uint32_t) {
    if (poll_window()) ++call_census()[target];
}

void census_dispatch(psprecomp::Runtime &, psprecomp::AllegrexContext &, std::uint32_t pc, std::int32_t) {
    if (poll_window()) ++call_census()[pc | 1u];  // low bit marks an outer dispatch
}

void report_census() {
    std::cerr << "[vc2-census] " << call_census().size() << " distinct targets in frames 600-602 (D = outer dispatch)\n";
    for (const auto &[target, count] : call_census()) {
        const std::uint32_t pc = target & ~1u;
        std::cerr << "[vc2-census] " << ((target & 1u) ? "D " : "C ") << "0x" << std::hex << pc << " elf=0x"
                  << (pc - 0x08804000u) << std::dec << " x" << count << "\n";
    }
}

void cri_trace_vblank(Runtime &rt, const AllegrexContext &ctx) {
    auto &memory = rt.memory();
    auto &state = cri_trace();
    ++state.frames;
    if (state.frames == 30u || state.frames % 300u == 0u) sample_frame_caller(rt, ctx, state.frames);
    if (state.frames == 300u || state.frames == 600u) report_histogram(rt, state.frames);
    poll_window() = state.frames >= 600u && state.frames < 603u;
    if (state.frames == 600u || state.frames == 603u)
        std::cerr << "[vc2-poll] ---- window " << (state.frames == 600u ? "open" : "closed") << " at frame " << state.frames << "\n";
    if (state.frames == 603u) report_census();
    if (state.frames == 600u || state.frames == 601u) dump_frame_list(rt);
    if (!memory.contains(vc2::at(kEuCriErrCallback), 0x10Cu)) return;

    const std::uint32_t callback = memory.load32(vc2::at(kEuCriErrCallback));
    if (callback == 0u) {
        memory.store32(vc2::at(kEuCriErrCallback), vc2::at(kEuCriCallbackStub));
        if (!state.announced_install) {
            std::cerr << "[vc2-cri] installed diagnostic error callback at frame " << state.frames
                      << " (suppress flag=" << memory.load32(vc2::at(kEuCriErrSuppress)) << ")\n";
            state.announced_install = true;
        }
    } else if (callback != vc2::at(kEuCriCallbackStub) && !state.announced_foreign) {
        std::cerr << "[vc2-cri] game registered its own error callback 0x" << std::hex << callback
                  << std::dec << " at frame " << state.frames << "\n";
        state.announced_foreign = true;
    }

    const std::uint32_t count = memory.load32(vc2::at(kEuCriErrCount));
    const std::uint32_t suppressed = memory.load32(vc2::at(kEuCriErrSuppressed));
    if (count != state.last_count || suppressed != state.last_suppressed) {
        std::cerr << "[vc2-cri] frame " << state.frames << " error count=" << count
                  << " suppressed=" << suppressed << " last formatted=\""
                  << read_cstring(memory, vc2::at(kEuCriErrFormatted), 256u) << "\"\n";
        state.last_count = count;
        state.last_suppressed = suppressed;
    }

    if (memory.contains(vc2::at(kEuCpkStartupBuffer), 32u)) {
        std::array<std::uint8_t, 32> now{};
        for (std::uint32_t i = 0; i < now.size(); ++i) now[i] = static_cast<std::uint8_t>(memory.load8(vc2::at(kEuCpkStartupBuffer) + i));
        if (!state.have_cpk || now != state.last_cpk) {
            std::cerr << "[vc2-cri] frame " << state.frames << " buf@0x" << std::hex << vc2::at(kEuCpkStartupBuffer)
                      << std::dec << " " << hex_bytes(now.data(), now.size()) << "\n";
            state.last_cpk = now;
            state.have_cpk = true;
        }
    }
}

void register_cri_trace(HleRegistrar &hle) {
    if (!env_set("TRACE_CRI")) return;
    psprecomp::set_runtime_pre_chained_call_hook(&census_chained);
    psprecomp::set_runtime_pre_dispatch_hook(&census_dispatch);

    HleFunction set_frame_buf = hle.bound_function("sceDisplay", 0x289D82FEu);
    if (set_frame_buf) {
        hle.bind("sceDisplay", 0x289D82FEu,
                 [inner = std::move(set_frame_buf)](Runtime &rt, AllegrexContext &ctx) mutable {
                     cri_trace_vblank(rt, ctx);
                     inner(rt, ctx);
                 });
    } else {
        std::cerr << "[vc2-cri] sceDisplaySetFrameBuf is not bound; CRI trace inactive\n";
        return;
    }

    // GE queue arguments. Steady-state VC2 frame lists are SIGNAL(2) + END;
    // log whether the game extends them through the stall address.
    struct GeWrap { const char *name; std::uint32_t nid; };
    for (const GeWrap wrap : {GeWrap{"sceGeListEnQueue", 0xAB49E76Au}, GeWrap{"sceGeListEnQueueHead", 0x1C0D95A6u},
                              GeWrap{"sceGeListUpdateStallAddr", 0xE0D68148u}, GeWrap{"sceGeListSync", 0x03444EB4u},
                              GeWrap{"sceGeSetCallback", 0xA4FC06A4u}, GeWrap{"sceGeListDeQueue", 0x5FB86AB0u},
                              GeWrap{"sceGeBreak", 0xB448EC0Du}, GeWrap{"sceGeContinue", 0x4C06E472u}}) {
        HleFunction inner = hle.bound_function("sceGe_user", wrap.nid);
        if (!inner) continue;
        hle.bind("sceGe_user", wrap.nid,
                 [inner = std::move(inner), name = wrap.name, calls = std::uint64_t{0}](Runtime &rt, AllegrexContext &ctx) mutable {
                     const std::uint32_t a0 = arg(ctx, 0u), a1 = arg(ctx, 1u), a2 = arg(ctx, 2u), a3 = arg(ctx, 3u);
                     const std::uint32_t ra = ctx.gpr[31];
                     inner(rt, ctx);
                     ++calls;
                     if (calls <= 24u || calls % 500u == 0u) {
                         std::cerr << "[vc2-ge] " << name << " #" << calls << " a0=0x" << std::hex << a0 << " a1=0x" << a1
                                   << " a2=0x" << a2 << " a3=0x" << a3 << " ra=0x" << ra << " -> 0x" << ctx.gpr[2];
                         if (std::string_view(name) == "sceGeSetCallback" && rt.memory().contains(a0, 16u))
                             std::cerr << " cbdata{sig=0x" << rt.memory().load32(a0) << " sigarg=0x" << rt.memory().load32(a0 + 4u)
                                       << " fin=0x" << rt.memory().load32(a0 + 8u) << " finarg=0x" << rt.memory().load32(a0 + 12u) << "}";
                         std::cerr << std::dec << " frame=" << cri_trace().frames << "\n";
                     }
                 });
    }

    // Steady-state waits, polls and status queries: what the main loop and its
    // workers check every frame, with call site and result (frames 600-602).
    struct PollWrap { const char *library; const char *name; std::uint32_t nid; };
    for (const PollWrap wrap : {
             PollWrap{"ThreadManForUser", "PollSema", 0x58B1F937u},
             PollWrap{"ThreadManForUser", "WaitSema", 0x4E3A1105u},
             PollWrap{"ThreadManForUser", "SignalSema", 0x3F53E640u},
             PollWrap{"ThreadManForUser", "ReferSemaStatus", 0xBC6FEBC5u},
             PollWrap{"ThreadManForUser", "PollEventFlag", 0x30FD48F0u},
             PollWrap{"ThreadManForUser", "WaitEventFlag", 0x402FCF22u},
             PollWrap{"ThreadManForUser", "WaitEventFlagCB", 0x328C546Au},
             PollWrap{"ThreadManForUser", "SetEventFlag", 0x1FB15A32u},
             PollWrap{"ThreadManForUser", "ClearEventFlag", 0x812346E4u},
             PollWrap{"ThreadManForUser", "ReferEventFlagStatus", 0xA66B0120u},
             PollWrap{"ThreadManForUser", "ReferThreadStatus", 0x17C1684Eu},
             PollWrap{"ThreadManForUser", "GetThreadExitStatus", 0x3B183E26u},
             PollWrap{"ThreadManForUser", "WaitThreadEnd", 0x278C0DF5u},
             PollWrap{"ThreadManForUser", "ReceiveMbx", 0x18260574u},
             PollWrap{"ThreadManForUser", "PollMbx", 0x0D81716Au},
             PollWrap{"ThreadManForUser", "SendMbx", 0xE9B3061Eu},
             PollWrap{"ThreadManForUser", "ReceiveMsgPipe", 0x74829B76u},
             PollWrap{"ThreadManForUser", "TryReceiveMsgPipe", 0xDF52098Fu},
             PollWrap{"ThreadManForUser", "CheckCallback", 0x349D6D6Cu},
             PollWrap{"ThreadManForUser", "DelayThread", 0xCEADEB47u},
             PollWrap{"ThreadManForUser", "DelayThreadCB", 0x68DA9E36u},
             PollWrap{"ThreadManForUser", "SleepThread", 0x9ACE131Eu},
             PollWrap{"ThreadManForUser", "SleepThreadCB", 0x82826F70u},
             PollWrap{"ThreadManForUser", "WakeupThread", 0xD59EAD2Fu},
             PollWrap{"IoFileMgrForUser", "IoPollAsync", 0x3251EA56u},
             PollWrap{"IoFileMgrForUser", "IoWaitAsync", 0xE23EEC33u},
             PollWrap{"IoFileMgrForUser", "IoWaitAsyncCB", 0x35DBD746u},
             PollWrap{"IoFileMgrForUser", "IoGetAsyncStat", 0xCB05F8D6u},
             PollWrap{"sceCtrl", "CtrlPeekBufferPositive", 0x3A622550u},
             PollWrap{"sceUtility", "SavedataGetStatus", 0x8874DBE0u},
             PollWrap{"sceUtility", "MsgDialogGetStatus", 0x9A1C91D7u},
             PollWrap{"sceUtility", "OskGetStatus", 0xF3F76017u},
             PollWrap{"sceUtility", "NetconfGetStatus", 0x6332AA39u},
             PollWrap{"sceUtility", "SavedataInitStart", 0x50C4CD57u},
             PollWrap{"sceUtility", "SavedataUpdate", 0xD4B95FFBu},
             PollWrap{"sceUtility", "SavedataShutdownStart", 0x9790B33Cu},
             PollWrap{"sceUtility", "MsgDialogInitStart", 0x2AD8E239u},
             PollWrap{"sceUtility", "MsgDialogUpdate", 0x95FC253Bu},
             PollWrap{"sceUtility", "GetSystemParamInt", 0xA5DA2406u},
         }) {
        HleFunction inner = hle.bound_function(wrap.library, wrap.nid);
        if (!inner) continue;
        hle.bind(wrap.library, wrap.nid,
                 [inner = std::move(inner), name = wrap.name](Runtime &rt, AllegrexContext &ctx) mutable {
                     if (!poll_window()) {
                         inner(rt, ctx);
                         return;
                     }
                     const std::uint32_t a0 = arg(ctx, 0u), a1 = arg(ctx, 1u), a2 = arg(ctx, 2u), a3 = arg(ctx, 3u);
                     const std::uint32_t ra = ctx.gpr[31];
                     const std::uint32_t sp = ctx.gpr[29];
                     inner(rt, ctx);
                     // A blocking call can switch threads; the stack pointer
                     // says whether ctx is still the caller's.
                     const bool same = ctx.gpr[29] == sp;
                     std::cerr << "[vc2-poll] f" << cri_trace().frames << " " << name << "(0x" << std::hex << a0 << ",0x" << a1
                               << ",0x" << a2 << ",0x" << a3 << ") ra=0x" << ra << " elf=0x" << (ra - 0x08804000u);
                     if (same) std::cerr << " -> 0x" << ctx.gpr[2];
                     else std::cerr << " -> (switched)";
                     std::cerr << std::dec << "\n";
                 });
    }

    HleFunction original = hle.bound_function("sceNetAdhocMatching", kCriCallbackNid);
    hle.bind("sceNetAdhocMatching", kCriCallbackNid,
             [original = std::move(original)](Runtime &rt, AllegrexContext &ctx) mutable {
                 if (ctx.gpr[31] != vc2::at(kEuCriNotifyReturn)) {
                     if (original) original(rt, ctx);
                     else kernel().finish(ctx, 0u);
                     return;
                 }
                 // criErr callback(message, p1, p2, p3).  The notifier saved
                 // its own caller's $ra at sp+20 (ELF 0x437884).
                 const std::uint32_t sp = ctx.gpr[29];
                 const std::uint32_t reporter = rt.memory().contains(sp + 20u, 4u) ? rt.memory().load32(sp + 20u) : 0u;
                 std::cerr << "[vc2-cri] error \"" << read_cstring(rt.memory(), arg(ctx, 0u), 256u)
                           << "\" p1=0x" << std::hex << arg(ctx, 1u) << " p2=0x" << arg(ctx, 2u)
                           << " p3=0x" << arg(ctx, 3u) << " reporter_ra=0x" << reporter
                           << " (elf 0x" << (reporter - 0x08804000u) << ")" << std::dec
                           << " frame=" << cri_trace().frames << "\n";
                 kernel().finish(ctx, 0u);
             });
    std::cerr << "[vc2-cri] trace armed: callback stub 0x" << std::hex << vc2::at(kEuCriCallbackStub)
              << ", globals 0x" << vc2::at(kEuCriErrCallback) << std::dec << "\n";
}

void store_float(psprecomp::GuestMemory &memory, std::uint32_t address, float value) {
    memory.store32(address, std::bit_cast<std::uint32_t>(value));
}

// SceFontStyle is 168 bytes. PortableKit exposes one host-backed font through
// sceFontGetNumFontList but does not yet implement the companion list query.
// VC2 reaches that query during native boot, so provide a fully-initialised
// record rather than allowing callers to read uninitialised guest memory.
void write_host_font_style(psprecomp::GuestMemory &memory, std::uint32_t address) {
    for (std::uint32_t offset = 0u; offset < kFontStyleBytes; ++offset) memory.store8(address + offset, 0u);
    store_float(memory, address + 0u, 0.0f);  // width: any size
    store_float(memory, address + 4u, 0.0f);  // height: any size
    memory.store16(address + 20u, 0u);        // SCE_FONT_FAMILY_DEFAULT
    memory.store16(address + 22u, 1u);        // SCE_FONT_STYLE_REGULAR
    memory.store16(address + 26u, 1u);        // SCE_FONT_LANGUAGE_JAPANESE
    memory.store16(address + 30u, 1u);        // SCE_FONT_VENDOR_COUNTRY_JAPAN
    constexpr char name[] = "PortableKit Host Font";
    for (std::uint32_t i = 0u; i < sizeof(name) - 1u; ++i) memory.store8(address + 32u + i, name[i]);
}

// --- On-demand GE frame dump (VC2_FRAME_DUMP=<directory>) ---------------------
// Press F9 in game: every display list enqueued during the next frame is
// walked from its start to its final stall address (or FINISH/END), and each
// primitive is written with the GE state that decides how it looks. This is
// for tying a visual fault (striped textures, grey shadows) to one draw and
// one GE feature before changing the renderer. Off unless the variable is set.
//
// Draw provenance (native-rendering plan, phase 0). VC2 writes every display
// list through a statically linked GU-style library whose state block sits at
// ELF segment 1 + 0x29978 (relocated to 0x08D5AEC0); its +0x48 field is the
// current GU context and that context's +8 is the list write pointer. While a
// frame is recorded, every chained (cross-unit / jalr) call and outer dispatch
// samples that pointer together with the call target and $ra. Each primitive is
// then attributed to the last observed call made before the writer reached it.
// Same-unit calls are gotos and stay invisible, so an attribution names the
// nearest observable caller, not necessarily the innermost function. Needs a
// build with PSPRECOMP_AOT_PRODUCTION_FASTPATHS off (the Debug capture build).
constexpr std::uint32_t kEuGuStateBlock = 0x08D5AEC0u;  // ELF seg1 + 0x29978 (European; .bss, the same in every edition)
constexpr std::uint32_t kGuContextField = 0x48u;
constexpr std::uint32_t kGuListWriteField = 0x08u;

struct DumpList {
    std::uint32_t id = 0u;
    std::uint32_t pc = 0u;
    std::uint32_t stall = 0u;
};

struct WriterSample {
    std::uint32_t target = 0u;  // function called (low bit set: outer dispatch)
    std::uint32_t ra = 0u;      // return address in the caller
};

struct FrameDumpState {
    std::string directory;
    bool key_down = false;
    bool armed = false;      // F9 seen: record the next whole frame
    bool recording = false;
    std::uint32_t frames_left = 0u;  // frames still to sample in this capture
    std::uint32_t sequence = 0u;
    std::vector<DumpList> lists;
    // List write pointer -> the latest call observed while the pointer was there.
    std::map<std::uint32_t, WriterSample> writers;
    std::uint64_t samples = 0u;
};

FrameDumpState &frame_dump() {
    static FrameDumpState state;
    return state;
}

bool f9_pressed() {
#if defined(PORTABLEKIT_HAS_SDL)
    int count = 0;
    const bool *keys = SDL_GetKeyboardState(&count);
    return keys != nullptr && SDL_SCANCODE_F9 < count && keys[SDL_SCANCODE_F9];
#else
    return false;
#endif
}

std::string hex8(std::uint32_t value) {
    char text[12];
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
}

void sample_writer(Runtime &rt, const AllegrexContext &ctx, std::uint32_t target) {
    auto &state = frame_dump();
    if (!state.recording || state.samples >= 4000000u) return;
    const auto &memory = rt.memory();
    if (!memory.contains(vc2::at(kEuGuStateBlock) + kGuContextField, 4u)) return;
    const std::uint32_t context = memory.load32(vc2::at(kEuGuStateBlock) + kGuContextField);
    if (context == 0u || !memory.contains(context + kGuListWriteField, 4u)) return;
    const std::uint32_t write = memory.load32(context + kGuListWriteField) & 0x0FFFFFFFu;
    state.writers[write] = WriterSample{target, ctx.gpr[31]};
    ++state.samples;
}

void provenance_chained(Runtime &rt, AllegrexContext &ctx, std::uint32_t target, std::uint32_t extra) {
    census_chained(rt, ctx, target, extra);
    sample_writer(rt, ctx, target);
}

void provenance_dispatch(Runtime &rt, AllegrexContext &ctx, std::uint32_t pc, std::int32_t extra) {
    census_dispatch(rt, ctx, pc, extra);
    sample_writer(rt, ctx, pc | 1u);
}

// The last call observed before the list writer reached `address`.
const WriterSample *writer_of(std::uint32_t address) {
    const auto &writers = frame_dump().writers;
    auto it = writers.upper_bound(address & 0x0FFFFFFFu);
    if (it == writers.begin()) return nullptr;
    --it;
    if ((address & 0x0FFFFFFFu) - it->first > 0x40000u) return nullptr;  // different buffer
    return &it->second;
}

// Walks one list like the GE: BASE/OFFSET/ORIGIN addressing, JUMP/CALL/RET,
// the SIGNAL list-flow forms, and END after an interrupting SIGNAL continuing.
void dump_list(std::ostream &out, const psprecomp::GuestMemory &memory, const DumpList &list,
               std::map<std::uint32_t, std::uint32_t> &by_writer) {
    std::array<std::uint32_t, 256> reg{};
    std::uint32_t pc = list.pc & 0x0FFFFFFFu;
    const std::uint32_t stall = list.stall & 0x0FFFFFFFu;
    std::uint32_t offset = 0u;
    std::vector<std::uint32_t> calls;
    std::uint32_t prims = 0u;
    const auto &writers = frame_dump().writers;
    const auto in_buffer = std::distance(writers.lower_bound(pc), writers.lower_bound(pc + 0x40000u));
    out << "list id=" << list.id << " pc=" << hex8(pc) << " stall=" << hex8(stall)
        << " writer samples in buffer=" << in_buffer << "\n";
    const auto target = [&](std::uint32_t data) {
        return ((((reg[0x10] & 0x000F0000u) << 8u) | (data & 0x00FFFFFCu)) + offset) & 0x0FFFFFFFu;
    };
    for (std::uint32_t steps = 0u; steps < 400000u; ++steps) {
        if (stall != 0u && pc == stall) { out << "  [stall reached]\n"; break; }
        if (!memory.contains(pc, 4u)) { out << "  [left memory at " << hex8(pc) << "]\n"; break; }
        const std::uint32_t word = memory.load32(pc);
        const std::uint32_t cmd = word >> 24u, data = word & 0x00FFFFFFu;
        const std::uint32_t here = pc;
        pc += 4u;
        if (cmd == 0x08u) { pc = target(data); continue; }
        if (cmd == 0x0Au) { calls.push_back(pc); pc = target(data); continue; }
        if (cmd == 0x0Bu) { if (!calls.empty()) { pc = calls.back(); calls.pop_back(); } continue; }
        if (cmd == 0x14u) { offset = here; continue; }
        if (cmd == 0x13u) { offset = data << 8u; continue; }
        if (cmd == 0x0Eu) {
            const std::uint32_t behaviour = data >> 16u;
            if (behaviour >= 0x10u && behaviour <= 0x12u && memory.contains(pc, 4u)) {
                const std::uint32_t t = (((data & 0xFFFFu) << 16u) | (memory.load32(pc) & 0xFFFFu)) & 0x0FFFFFFCu;
                pc += 4u;
                if (behaviour == 0x10u) pc = t;
                else if (behaviour == 0x11u) { calls.push_back(pc); pc = t; }
                else if (!calls.empty()) { pc = calls.back(); calls.pop_back(); }
            }
            out << "  " << hex8(here) << " SIGNAL " << hex8(word) << "\n";
            continue;
        }
        if (cmd == 0x0Cu) {
            const std::uint32_t prior = memory.contains(here - 4u, 4u) ? memory.load32(here - 4u) : 0u;
            if ((prior >> 24u) == 0x0Eu && (((prior >> 16u) & 0xFFu) == 1u || ((prior >> 16u) & 0xFFu) == 2u)) continue;
            out << "  END at " << hex8(here) << "\n";
            break;
        }
        if (cmd == 0x0Fu) { out << "  FINISH at " << hex8(here) << "\n"; continue; }
        if (cmd == 0x04u || cmd == 0x05u || cmd == 0x06u) {
            ++prims;
            const auto r = [&](std::uint32_t c) { return hex8(reg[c] & 0x00FFFFFFu).substr(2); };
            const std::uint32_t tex = ((reg[0xA8] & 0x000F0000u) << 8u) | (reg[0xA0] & 0x00FFFFFFu);
            const std::uint32_t clut = ((reg[0xB1] & 0x000F0000u) << 8u) | (reg[0xB0] & 0x00FFFFFFu);
            out << "  " << hex8(here) << " " << (cmd == 0x04u ? "PRIM" : cmd == 0x05u ? "BEZIER" : "SPLINE")
                << " type=" << ((data >> 16u) & 7u) << " n=" << (data & 0xFFFFu) << " vtype=" << r(0x12)
                << " fb=" << r(0x9C) << "/" << (reg[0x9D] & 0x7FFu) << " psm=" << (reg[0xD2] & 3u)
                << " | TME=" << (reg[0x1E] & 1u);
            if (reg[0x1E] & 1u)
                out << " tex=" << hex8(tex) << " bw=" << (reg[0xA8] & 0xFFFFu) << " size=" << r(0xB8)
                    << " tpsm=" << (reg[0xC3] & 0xFu) << " tmode=" << r(0xC2) << " clut=" << hex8(clut)
                    << " cmode=" << r(0xC5) << " tfunc=" << r(0xC9) << " tflt=" << r(0xC6) << " wrap=" << r(0xC7)
                    << " TMAP=" << r(0xC0) << " TSHADE=" << r(0xC1) << " TBIAS=" << r(0xC8)
                    << " uvs=" << r(0x48) << "," << r(0x49) << " uvo=" << r(0x4A) << "," << r(0x4B);
            out << " | ABE=" << (reg[0x21] & 1u) << " blend=" << r(0xDF) << " fix=" << r(0xE0) << "/" << r(0xE1)
                << " ATE=" << (reg[0x22] & 1u) << " atst=" << r(0xDB) << " ZTE=" << (reg[0x23] & 1u)
                << " ztst=" << r(0xDE) << " zmsk=" << (reg[0xE7] & 1u) << " | STE=" << (reg[0x24] & 1u)
                << " stst=" << r(0xDC) << " sop=" << r(0xDD) << " | pmsk=" << r(0xE8) << "/" << r(0xE9)
                << " CTE=" << (reg[0x27] & 1u) << " LOE=" << (reg[0x28] & 1u) << " lop=" << r(0xE6)
                << " LTE=" << (reg[0x17] & 1u) << " FGE=" << (reg[0x1F] & 1u);
            // Where it lands: the vertices' position and texture-coordinate
            // ranges (through mode: screen pixels; otherwise the game's own
            // space, which for 2D is close to them) and the first colour.
            {
                const std::uint32_t vtype = reg[0x12] & 0x00FFFFFFu, count = data & 0xFFFFu;
                const std::uint32_t base = (reg[0x10] & 0x000F0000u) << 8u;
                const std::uint32_t vaddr = (base | (reg[0x01] & 0x00FFFFFFu)) & 0x0FFFFFFFu;
                const std::uint32_t iaddr = (base | (reg[0x02] & 0x00FFFFFFu)) & 0x0FFFFFFFu;
                const std::uint32_t itype = (vtype >> 11u) & 3u;
                std::uint32_t needed = count;
                if (itype != 0u) {
                    const std::uint32_t size = itype == 1u ? 1u : itype == 2u ? 2u : 4u;
                    needed = 0u;
                    for (std::uint32_t i = 0; i < count && memory.contains(iaddr + i * size, size); ++i) {
                        const std::uint32_t index = size == 1u ? memory.load8(iaddr + i)
                                                    : size == 2u ? memory.load16(iaddr + i * 2u)
                                                                 : memory.load32(iaddr + i * 4u);
                        needed = std::max(needed, index + 1u);
                    }
                }
                std::vector<gpu::Vertex> vertices;
                if (needed != 0u && needed < 65536u && memory.contains(vaddr, 4u))
                    gpu::decode_vertices(memory, vaddr, vtype, needed, vertices);
                if (!vertices.empty()) {
                    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
                    for (const gpu::Vertex &v : vertices) {
                        x0 = std::min(x0, v.position[0]); x1 = std::max(x1, v.position[0]);
                        y0 = std::min(y0, v.position[1]); y1 = std::max(y1, v.position[1]);
                        u0 = std::min(u0, v.texcoord[0]); u1 = std::max(u1, v.texcoord[0]);
                        v0 = std::min(v0, v.texcoord[1]); v1 = std::max(v1, v.texcoord[1]);
                    }
                    char where[200];
                    std::snprintf(where, sizeof(where), " | at x=%.1f..%.1f y=%.1f..%.1f uv=%.4f..%.4f,%.4f..%.4f color0=%08X",
                                  x0, x1, y0, y1, u0, u1, v0, v1, vertices[0].color);
                    out << where;
                }
            }
            if (const WriterSample *w = writer_of(here)) {
                out << " | by=" << ((w->target & 1u) ? "D:" : "") << hex8(w->target & ~1u) << " ra=" << hex8(w->ra);
                ++by_writer[w->target];
            } else {
                out << " | by=?";
                ++by_writer[0u];
            }
            out << "\n";
            if (prims >= 3000u) { out << "  [primitive limit]\n"; break; }
            continue;
        }
        reg[cmd] = word;
    }
    out << "  primitives=" << prims << "\n";
}

void finish_frame_dump(Runtime &rt) {
    auto &state = frame_dump();
    const std::string path = state.directory + "/frame-dump-" + std::to_string(++state.sequence) + ".txt";
    std::ofstream out(path);
    out << "VC2 GE frame dump " << state.sequence << ": " << state.lists.size() << " list(s), "
        << state.samples << " writer samples\n";
    std::map<std::uint32_t, std::uint32_t> by_writer;
    for (const auto &list : state.lists) dump_list(out, rt.memory(), list, by_writer);
    // Primitives per attributed caller, largest first (D: = outer dispatch).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranked(by_writer.begin(), by_writer.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    out << "primitives by attributed caller:\n";
    for (const auto &[target, count] : ranked) {
        if (target == 0u) { out << "  ?            x" << count << "\n"; continue; }
        out << "  " << ((target & 1u) ? "D:" : "  ") << hex8(target & ~1u) << " elf=" << hex8((target & ~1u) - 0x08804000u)
            << " x" << count << "\n";
    }
    std::cerr << "[vc2-dump] wrote " << path << " (" << state.lists.size() << " lists)\n";
    state.lists.clear();
    state.writers.clear();
    state.samples = 0u;
}

void register_frame_dump(HleRegistrar &hle) {
    const char *directory = env("FRAME_DUMP");
    if (directory == nullptr || *directory == '\0') return;
    frame_dump().directory = directory;
    // Draw provenance hooks; they also feed the VC2_TRACE_CRI census, which
    // would otherwise own these single hook slots.
    psprecomp::set_runtime_pre_chained_call_hook(&provenance_chained);
    psprecomp::set_runtime_pre_dispatch_hook(&provenance_dispatch);

    HleFunction set_frame_buf = hle.bound_function("sceDisplay", 0x289D82FEu);
    if (set_frame_buf)
        hle.bind("sceDisplay", 0x289D82FEu, [inner = std::move(set_frame_buf)](Runtime &rt, AllegrexContext &ctx) mutable {
            // VC2 double-buffers its frame list: the list submitted in one frame
            // was written during the frame before. A capture therefore samples
            // the writer for two frames and dumps the lists submitted in the
            // second, whose construction the first frame's samples covered.
            auto &state = frame_dump();
            if (state.recording) {
                if (--state.frames_left == 0u) {
                    state.recording = false;
                    finish_frame_dump(rt);
                } else {
                    state.lists.clear();  // keep the writer samples
                }
            }
            if (state.armed) {
                state.armed = false;
                state.recording = true;
                state.frames_left = 2u;
                state.lists.clear();
                state.writers.clear();
                state.samples = 0u;
            }
            const bool down = f9_pressed();
            if (down && !state.key_down) {
                state.armed = true;
                std::cerr << "[vc2-dump] F9: recording the next two frames\n";
            }
            state.key_down = down;
            inner(rt, ctx);
        });

    for (const std::uint32_t nid : {0xAB49E76Au, 0x1C0D95A6u}) {  // sceGeListEnQueue(Head)
        HleFunction inner = hle.bound_function("sceGe_user", nid);
        if (!inner) continue;
        hle.bind("sceGe_user", nid, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            const std::uint32_t pc = arg(ctx, 0u), stall = arg(ctx, 1u);
            inner(rt, ctx);
            if (frame_dump().recording) frame_dump().lists.push_back({ctx.gpr[2], pc, stall});
        });
    }
    HleFunction update = hle.bound_function("sceGe_user", 0xE0D68148u);  // sceGeListUpdateStallAddr
    if (update)
        hle.bind("sceGe_user", 0xE0D68148u, [inner = std::move(update)](Runtime &rt, AllegrexContext &ctx) mutable {
            const std::uint32_t id = arg(ctx, 0u), stall = arg(ctx, 1u);
            inner(rt, ctx);
            for (auto &list : frame_dump().lists)
                if (list.id == id) list.stall = stall;
        });
    std::cerr << "[vc2-dump] F9 writes a GE frame dump to " << directory << "\n";
}

// --- Frame timing trace (VC2_TRACE_TIMING) -------------------------------------
// Native-rendering plan, HFR groundwork: the game's own update rate in each
// mode, measured rather than assumed. sceDisplaySetFrameBuf alone is not a
// frame count (VC2 calls it every vblank, often re-presenting the same buffer,
// and several threads wait for vblank), so each report, every 120 flip calls,
// gives three rates over the same wall-clock window:
//   flip calls/s     sceDisplaySetFrameBuf calls
//   new frames/s     flip calls whose framebuffer address differs from the last
//   lists/s          display lists submitted (sceGeListEnQueue/EnQueueHead)
// plus vblank waits per flip call, across all threads.
struct TimingTrace {
    std::uint64_t flips = 0u;
    std::uint64_t window_waits = 0u;
    std::uint64_t window_new_frames = 0u;
    std::uint64_t window_lists = 0u;
    std::uint32_t last_buffer = 0u;
    std::chrono::steady_clock::time_point window_start{};
};

TimingTrace &timing_trace() {
    static TimingTrace state;
    return state;
}

void register_timing_trace(HleRegistrar &hle) {
    if (!env_set("TRACE_TIMING")) return;
    for (const std::uint32_t nid : {0x984C27E7u, 0x46F186C3u}) {  // sceDisplayWaitVblankStart(CB)
        HleFunction inner = hle.bound_function("sceDisplay", nid);
        if (!inner) continue;
        hle.bind("sceDisplay", nid, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            ++timing_trace().window_waits;
            inner(rt, ctx);
        });
    }
    for (const std::uint32_t nid : {0xAB49E76Au, 0x1C0D95A6u}) {  // sceGeListEnQueue(Head)
        HleFunction inner = hle.bound_function("sceGe_user", nid);
        if (!inner) continue;
        hle.bind("sceGe_user", nid, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            ++timing_trace().window_lists;
            inner(rt, ctx);
        });
    }
    HleFunction set_frame_buf = hle.bound_function("sceDisplay", 0x289D82FEu);
    if (!set_frame_buf) return;
    hle.bind("sceDisplay", 0x289D82FEu, [inner = std::move(set_frame_buf)](Runtime &rt, AllegrexContext &ctx) mutable {
        auto &state = timing_trace();
        const std::uint32_t buffer = ctx.gpr[4];
        if (buffer != 0u && buffer != state.last_buffer) {
            ++state.window_new_frames;
            state.last_buffer = buffer;
        }
        const auto now = std::chrono::steady_clock::now();
        if (state.flips == 0u) state.window_start = now;
        if (++state.flips % 120u == 0u) {
            const double seconds = std::chrono::duration<double>(now - state.window_start).count();
            const auto rate = [seconds](std::uint64_t count) { return seconds > 0.0 ? count / seconds : 0.0; };
            std::cerr << "[vc2-timing] flips=" << state.flips << std::fixed << std::setprecision(1)
                      << " flip calls/s=" << rate(120u) << " new frames/s=" << rate(state.window_new_frames)
                      << " lists/s=" << rate(state.window_lists) << std::setprecision(2)
                      << " vblank waits/flip=" << static_cast<double>(state.window_waits) / 120.0 << "\n"
                      << std::defaultfloat;
            state.window_start = now;
            state.window_waits = state.window_new_frames = state.window_lists = 0u;
        }
        inner(rt, ctx);
    });
    std::cerr << "[vc2-timing] reporting every 120 flip calls\n";
}

// --- Native GU replacements (VC2_NATIVE_GU=<names>): see vc2_native_gu.cpp ------
void patch_loaded_image(Runtime &rt, const psprecomp::Elf32Image &) {
    vc2::bind_corpus_hooks();
    vc2::install_native_gu(rt);
    vc2::install_prompts();
    vc2::install_title_settings();
    vc2::install_tick(rt);
    vc2::install_text(rt);
    vc2::install_extras(rt);
    vc2::install_cheats(rt);
}

// PortableKit's fast loading (emulated time running ahead of real time while
// the game loads) is off for VC2: its 2-16x bursts around transitions threw off
// the high-frame-rate pacing and measurements, and the loads are short anyway
// (user's decision, 2026-11-08).
bool fast_loading_allowed() { return false; }

void register_extra_hle(HleRegistrar &hle) {
    vc2::options::load();
    vc2::start_discord();
    vc2::install_audio(hle);
    vc2::install_voice();
    vc2::install_online();
    // A PC has no wireless switch: it is always on, and the game's ad hoc menus
    // never stop at "The WLAN/WIRELESS switch is off" (2026-11-14).
    hle.bind("sceWlanDrv", 0xD7763699u, [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 1u); });
    // Going on line. With online play set in the launcher (vc2_online.cpp), the
    // session is the online one. Otherwise, with no session to be in, this
    // player hosts one on this PC (the built-in server), or, when this PC
    // already hosts one (another copy of the game), joins it.
    if (HleFunction inner = hle.bound_function("sceNetAdhocctl", 0xE26F226Eu)) {  // sceNetAdhocctlInit
        hle.bind("sceNetAdhocctl", 0xE26F226Eu, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            const settings::Settings &s = settings::current();
            if (vc2::online_prepare_adhoc()) {
                // set up by online play
            } else if (!adhoc_hosting() && (!s.adhoc || s.adhoc_server.empty())) {
                if (adhoc_host_start()) {
                    std::cerr << "[vc2-online] no session set: hosting one\n";
                } else {
                    std::cerr << "[vc2-online] cannot host (" << adhoc_host_error()
                              << "); joining the session on this PC\n";
                    adhoc_join("127.0.0.1");
                }
            }
            inner(rt, ctx);
        });
    }
    // The saving indicator (vc2_native_gu.cpp): the save call's write modes
    // (AUTOSAVE, SAVE, LISTSAVE, MAKEDATA[SECURE], WRITEDATA[SECURE]; the mode
    // is the word at +0x30 of its parameters).
    if (HleFunction inner = hle.bound_function("sceUtility", 0x50C4CD57u)) {
        hle.bind("sceUtility", 0x50C4CD57u, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            if (const std::uint32_t params = arg(ctx, 0u); params != 0u && rt.memory().contains(params, 0x34u)) {
                const std::uint32_t mode = rt.memory().load32(params + 0x30u);
                if (mode == 1u || mode == 3u || mode == 5u || mode == 13u || mode == 14u || mode == 17u || mode == 18u)
                    vc2::note_save_write();
            }
            inner(rt, ctx);
        });
    }
#if defined(PORTABLEKIT_HAS_RENDERER)
    // PortableKit's in-game settings menu (Esc, L3+R3, touch button) is detached:
    // the port is configured by its own launcher. VC2_PORTABLEKIT_MENU=1 brings
    // it back for debugging.
    // Its overlays go with it: the first-run "Esc / L3+R3 opens the menu"
    // hint and the touch-screen buttons (only the FPS counter, ours, stays).
    if (env("PORTABLEKIT_MENU") == nullptr) {
        ui::set_menu_enabled(false);
        settings::current().menu_hint_seen = true;
        settings::current().touch_controls = false;
    }
#endif
    register_cri_trace(hle);
    register_frame_dump(hle);
    register_timing_trace(hle);
    vc2::register_native_gu_report(hle);
    vc2::register_font(hle);
    // Before the census, so its pad sampling sees the remapped buttons.
    vc2::register_controls(hle);
    vc2::register_census(hle);
    vc2::register_tick(hle);

    // sceKernelLockMutexCB has the same uncontended behavior as the framework
    // mutex lock.  Strict traces must later prove whether VC2 needs callback
    // delivery while it is blocked by another thread.
    HleFunction lock = hle.bound_function("ThreadManForUser", 0xB011B11Fu);
    if (lock) hle.bind("ThreadManForUser", 0x5BF4DD27u, std::move(lock));

    // A missing ms0 directory can be either an optional content scan or a
    // boot prerequisite. Keep the framework's I/O behavior unchanged, but
    // let a bounded VC2 trace identify the caller and its returned result.
    // This is off unless explicitly requested and is useful when comparing a
    // PSP reference run or carrying the diagnostic method to VC3.
    HleFunction dopen = hle.bound_function("IoFileMgrForUser", 0xB29DDF9Cu);
    if (dopen) {
        hle.bind("IoFileMgrForUser", 0xB29DDF9Cu,
                 [dopen = std::move(dopen)](Runtime &rt, AllegrexContext &ctx) mutable {
                     const bool trace = env_set("TRACE_STARTUP_IO");
                     const std::string path = trace ? read_cstring(rt.memory(), arg(ctx, 0u), 256u) : std::string{};
                     const std::uint32_t caller = ctx.gpr[31];
                     dopen(rt, ctx);
                     if (trace)
                         std::cerr << "[vc2-io] dopen " << path << " caller=0x" << std::hex << caller
                                   << " result=0x" << ctx.gpr[2] << std::dec << "\n";
                 });
    }

    // pspUmdInfo is { size, type }; this checked ULES01417 disc is a game
    // UMD, whose documented type is 0x10.  The trace supplies an eight-byte
    // writable structure at a0.
    hle.bind("sceUmdUser", 0x340B7686u, [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t info = arg(ctx, 0u);
        if (info != 0u && rt.memory().load32(info) >= 8u) rt.memory().store32(info + 4u, 0x10u);
        kernel().finish(ctx, 0u);
    });

    // int sceFontGetFontList(void *library, SceFontStyle *output, int size).
    // The built-in font HLE supplies exactly one virtual host font. Return the
    // number copied and never write an incomplete SceFontStyle.
    hle.bind("sceLibFont", 0xBC75D85Bu, [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t output = arg(ctx, 1u);
        const auto capacity = static_cast<std::int32_t>(arg(ctx, 2u));
        const bool trace = env_set("TRACE_FONT_LIST");
        if (capacity > 0 && output != 0u && rt.memory().contains(output, kFontStyleBytes)) {
            write_host_font_style(rt.memory(), output);
            if (trace)
                std::cerr << "[vc2-font] GetFontList lib=0x" << std::hex << arg(ctx, 0u)
                          << " output=0x" << output << " capacity=" << std::dec << capacity
                          << " caller=0x" << std::hex << ctx.gpr[31] << " -> 1\n" << std::dec;
            kernel().finish(ctx, 1u);
            return;
        }
        if (trace)
            std::cerr << "[vc2-font] GetFontList output=0x" << std::hex << output << " capacity=" << std::dec
                      << capacity << " caller=0x" << std::hex << ctx.gpr[31] << " -> 0\n" << std::dec;
        kernel().finish(ctx, 0u);
    });
}

} // namespace

// The US release (ULUS10515). Its executable is the European one with a
// two-call change in one function; every address the profile uses is
// translated by vc2::at() (vc2_region.hpp, research/REGIONS.md). Its corpus is
// built from generated-variants/us/. The hooks are the profile's own.
constexpr ProfileVariant kVariants[] = {
    {
        .key = "us",
        .name = "US release (ULUS10515)",
        .encrypted_executable_sha256 = "7ce669b391087cac5e6af58bc847f0b91c96a4e10d9cef6d89c577eb44cca642",
        .executable_sha256 = "f016e74871eec117954aba4e42738741c5d743d0bb6cfcc51f89714c1c0944af",
        .disc_id = "ULUS10515",
        .disc_id_display = "ULUS-10515",
        .release_name = "the North American release",
        .adhoc_product_code = "ULUS10515",
    },
    // The Japanese release (NPJH50145, disc version 1.01). A separate build
    // under its own tag and key; its addresses are an explicit table in
    // vc2_region.cpp. A Japanese console reports Japanese and circle to
    // confirm. Its corpus is built from generated-variants/jp/.
    {
        .key = "jp",
        .name = "Japanese release (NPJH50145)",
        .encrypted_executable_sha256 = "5334d22fc12bdc52d078877e2d99df1c3cceb922b8890ff9426ec4b7a0fb539d",
        .executable_sha256 = "582381436a2afadf94b4b210dc83ae338782da746102612f85fd4b2fc03f09a3",
        .disc_id = "NPJH50145",
        .disc_id_display = "NPJH-50145",
        .release_name = "the Japanese release",
        .adhoc_product_code = "NPJH50145",
        // Tag-key material is a PSP file-format constant for tag D91612F0;
        // the installer checks both hashes.
        .decryption_tag = 0xD91612F0u,
        .decryption_key = {0x9Eu, 0x20u, 0xE1u, 0xCDu, 0xD7u, 0x88u, 0xDEu, 0xC0u,
                           0x31u, 0x9Bu, 0x10u, 0xAFu, 0xC5u, 0xB8u, 0x73u, 0x23u},
        .system_language = 0,
        .confirm_button = 0,
    },
};

const GameProfile &game() {
    static const GameProfile profile{
        .app_name = "Valkyria Chronicles II",  // window title and logs; the data folder is set apart below
        .project_name = "Valkyria Chronicles II",
        .env_prefix = "VC2",
        .data_organization = "LansealAcademy",
        .data_application = "VC2Native",
        .disc_id = "ULES01417",
        .disc_id_display = "ULES-01417",
        .game_title = "Valkyria Chronicles II",
        .executable_path_on_disc = "PSP_GAME/SYSDIR/EBOOT.BIN",
        .param_sfo_path_on_disc = "PSP_GAME/PARAM.SFO",
        .release_name = "the European release",
        .encrypted_executable_sha256 = "98b4c1626f7d36bffae280f904bef4d718a5ea49090966c109731cebfe824211",
        .executable_sha256 = "5bc43ae81b3a35f2fe4a72180e7130598f3b856d7fb849ec7cdc49646187cefe",
        .decryption_tag = 0xD91613F0u,
        // Tag-key material is a PSP file-format constant for tag D91613F0.
        // The installer independently checks both the encrypted and prepared
        // executable hashes before accepting a user's disc image.
        .decryption_key = {0xEBu, 0xFFu, 0x40u, 0xD8u, 0xB4u, 0x1Au, 0xE1u, 0x66u,
                           0x91u, 0x3Bu, 0x8Fu, 0x64u, 0xB6u, 0xFCu, 0xB7u, 0x12u},
        .variants = kVariants,
        .load_base = 0x08804000u,
        .guest_ram_bytes = 32u * 1024u * 1024u,
        .boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN",
        .overlay_slots = {},
        .save_game_name = "ULES01417DATA",
        .save_folders = kSaveFolders,
        .adhoc_product_code = "ULES01417",
        .register_extra_hle = &register_extra_hle,
        .patch_loaded_image = &patch_loaded_image,
        .camera = &vc2::kCameraDriver,
        .view_aspect_frame = &vc2::view_aspect_frame,
        .fast_loading_allowed = &fast_loading_allowed,
        .system_language = 1u,
        .confirm_button = 1u,
        .soundtrack_music = &::vc2::soundtrack_music,
        .soundtrack_table = &::vc2::soundtrack_table,
    };
    return profile;
}

} // namespace portablekit
