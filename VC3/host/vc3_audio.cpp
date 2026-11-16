// The game's sound volumes from the launcher (settings.ini), and back (VC2's
// vc2_audio.cpp, measured again in VC3, 2026-11-15).
//
// OPTIONS has Music, SFX and Voice volumes, each 0 to 10. They sit in the
// game's options block, VC3 0x08D5BE88 (VC2 Europe 0x08D5DA08, at() maps it;
// 176 bytes, the same layout as VC2's): +36 music, +40 SFX, +44 voice, with
// defaults 6, 9 and 10 (VC3 0x08814CCC at boot, 0x08814E28 for OPTIONS'
// Default). The game hands them to its sound manager (the pointer at VC3
// 0x08CF1774, VC2 0x08CF9A7C) as value / 10 in floats at +9144, +9148 and
// +9152 (VC2: +9064..), with change bits 1 | 2 | 4 in its word at +4.
// OPTIONS edits a copy (its object +0x78) and plays each slider on the way
// (0x08AC6ECC); closing copies it back and applies (0x08AC2A60); Default
// resets the options block itself and applies (0x08AC3B18).
//
// settings.ini: vc3.music_volume, vc3.sfx_volume, vc3.voice_volume = 0..10.
//   - Checked once a frame (at sceDisplaySetFrameBuf, on the game's thread):
//     a key that is set wins whenever the game's value changes on its own (the
//     defaults at boot, the system data loading); the game's value and its
//     sound manager are set to it. A key not set leaves the game's own.
//   - OPTIONS closing (cmake: vc3_audio_options_closed at L_08AC2A60): the
//     values are the player's and go to settings.ini, so the launcher shows
//     them. From Default (vc3_audio_options_default at L_08AC3B18) until then
//     the once-a-frame check leaves the game's values alone.
// Logs [vc3-audio].

#include "vc3_audio.hpp"
#include "vc3_region.hpp"
#include "vc3_settings.hpp"

#include "hle/hle_common.hpp"
#include "kernel/kernel.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <string>

namespace portablekit::vc3 {
namespace {

constexpr std::uint32_t kEuOptions = 0x08D5DA08u;       // VC3 0x08D5BE88 through at()
constexpr std::uint32_t kEuSoundManager = 0x08CF9A7Cu;  // a pointer; VC3 0x08CF1774
constexpr std::uint32_t kSoundManagerChanged = 4u;       // its change bits
constexpr std::uint32_t kSoundManagerVolumes = 9144u;    // three floats (VC2: 9064)

struct Volume {
    options::Key key;
    const char *name;
    std::uint32_t offset;  // in the game's options
};
constexpr std::array<Volume, 3> kVolumes{{
    {options::Key::MusicVolume, "music", 36u},
    {options::Key::SfxVolume, "SFX", 40u},
    {options::Key::VoiceVolume, "voice", 44u},
}};

struct State {
    bool installed = false;
    bool in_options = false;              // OPTIONS' Default was used; closing decides
    std::array<int, 3> last{-1, -1, -1};  // the game's values at the last check
};
State &state() {
    static State s;
    return s;
}

int setting_value(options::Key key) {
    const std::string &text = options::get(key);
    if (text.empty()) return -1;
    return std::stoi(text);  // options validated it: 0..10
}

// The game's own steps when its volumes change: the value, then its sound
// manager's copy of it (value / 10) with the change bits.
void set_game_volume(psprecomp::GuestMemory &memory, std::size_t i, int value) {
    memory.store32(at(kEuOptions) + kVolumes[i].offset, static_cast<std::uint32_t>(value));
    const std::uint32_t manager = memory.load32(at(kEuSoundManager));
    if (manager == 0u || !memory.contains(manager, kSoundManagerVolumes + 12u)) return;
    memory.store32(manager + kSoundManagerChanged, memory.load32(manager + kSoundManagerChanged) | 7u);
    const float level = static_cast<float>(value) / 10.0f;
    memory.store32(manager + kSoundManagerVolumes + 4u * static_cast<std::uint32_t>(i), std::bit_cast<std::uint32_t>(level));
}

void check(Runtime &rt) {
    State &s = state();
    if (s.in_options) return;
    auto &memory = rt.memory();
    const std::uint32_t base = at(kEuOptions);
    if (base == 0u || !memory.contains(base, 48u)) return;
    for (std::size_t i = 0; i < kVolumes.size(); ++i) {
        const auto now = static_cast<int>(memory.load32(base + kVolumes[i].offset));
        if (now == s.last[i]) continue;
        const int wanted = setting_value(kVolumes[i].key);
        if (wanted >= 0 && wanted != now) {
            set_game_volume(memory, i, wanted);
            std::cerr << "[vc3-audio] " << kVolumes[i].name << " volume " << wanted << " (settings.ini; the game had "
                      << now << ")\n";
            s.last[i] = wanted;
            continue;
        }
        s.last[i] = now;
    }
}

}  // namespace

void install_audio(HleRegistrar &hle) {
    State &s = state();
    if (s.installed) return;
    HleFunction inner = hle.bound_function("sceDisplay", 0x289D82FEu);  // sceDisplaySetFrameBuf
    if (!inner) {
        std::cerr << "[vc3-audio] sceDisplaySetFrameBuf not bound: volumes follow the game only\n";
        return;
    }
    s.installed = true;
    hle.bind("sceDisplay", 0x289D82FEu, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
        check(rt);
        inner(rt, ctx);
    });
    std::cerr << "[vc3-audio] music " << options::get(options::Key::MusicVolume) << ", SFX "
              << options::get(options::Key::SfxVolume) << ", voice " << options::get(options::Key::VoiceVolume)
              << " (empty: the game's own)\n";
}

}  // namespace portablekit::vc3

// OPTIONS' Default reset the options block: the player's, until OPTIONS closes.
extern "C" void vc3_audio_options_default() noexcept {
    portablekit::vc3::state().in_options = true;
    std::cerr << "[vc3-audio] OPTIONS: Default\n";
}

// OPTIONS closed: its volumes are the player's; they go to settings.ini.
extern "C" void vc3_audio_options_closed() noexcept {
    using namespace portablekit::vc3;
    State &s = state();
    s.in_options = false;
    auto &memory = portablekit::kernel().runtime().memory();
    const std::uint32_t base = at(kEuOptions);
    if (base == 0u || !memory.contains(base, 48u)) return;
    bool adopted = false;
    for (std::size_t i = 0; i < kVolumes.size(); ++i) {
        const auto now = static_cast<int>(memory.load32(base + kVolumes[i].offset));
        s.last[i] = now;
        if (now < 0 || now > 10 || std::to_string(now) == options::get(kVolumes[i].key)) continue;
        if (options::set(kVolumes[i].key, std::to_string(now))) {
            adopted = true;
            std::cerr << "[vc3-audio] " << kVolumes[i].name << " volume " << now << " (OPTIONS) -> settings.ini\n";
        }
    }
    if (adopted && !options::save()) std::cerr << "[vc3-audio] the volumes were not saved to settings.ini\n";
}
