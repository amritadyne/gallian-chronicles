// The game's sound volumes from the launcher (settings.ini), and back.
//
// OPTIONS > Preferences has Music Volume, SFX Volume and Voice Volume, each
// 0 to 10 (the sliders are made at EU 0x08B40778 with the range 0..10). The
// values sit in the game's options at EU 0x08D5DA08 (176 bytes, also the
// camera inverts: vc2_camera.cpp): +36 music, +40 SFX, +44 voice. Their
// defaults (EU 0x08813EDC) are 6, 9 and 10. The game hands them to its sound
// manager (the pointer at EU 0x08CF9A7C) as value / 10 in floats at +9064,
// +9068 and +9072, with change bits 1 | 2 | 4 in its word at +4: after the
// system data loads (EU 0x08B83D54), when OPTIONS closes (EU 0x08B3E244) and
// when its changes are discarded (EU 0x08B3F2B4). The system data load copies
// the options from the save (EU 0x08B97648); saving copies them back.
//
// settings.ini: vc2.music_volume, vc2.sfx_volume, vc2.voice_volume = 0..10.
// Checked once a frame (at sceDisplaySetFrameBuf, on the game's thread):
//   - a key that is set wins over the game's value whenever that value changes
//     on its own (the system data loading, the defaults being set at boot);
//     the game's value and its sound manager are set to it.
//   - a change made after the OPTIONS screen opened is the player's: it is
//     written to settings.ini, so the launcher shows it.
//   - a key that is not set leaves the game's value alone (the save's), until
//     the player changes it in OPTIONS.
// Logs [vc2-audio].

#include "vc2_audio.hpp"
#include "vc2_region.hpp"
#include "vc2_settings.hpp"

#include "hle/hle_common.hpp"
#include "kernel/kernel.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <string>

namespace portablekit::vc2 {
namespace {

constexpr std::uint32_t kEuOptions = 0x08D5DA08u;
constexpr std::uint32_t kEuSoundManager = 0x08CF9A7Cu;  // a pointer
constexpr std::uint32_t kSoundManagerChanged = 4u;       // its change bits
constexpr std::uint32_t kSoundManagerVolumes = 9064u;    // three floats

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
    bool options_opened = false;
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
    auto &memory = rt.memory();
    const std::uint32_t base = at(kEuOptions);
    if (base == 0u || !memory.contains(base, 48u)) return;
    bool adopted = false;
    for (std::size_t i = 0; i < kVolumes.size(); ++i) {
        const auto now = static_cast<int>(memory.load32(base + kVolumes[i].offset));
        if (now == s.last[i]) continue;
        const int wanted = setting_value(kVolumes[i].key);
        if (s.options_opened && s.last[i] >= 0 && now >= 0 && now <= 10) {
            if (options::set(kVolumes[i].key, std::to_string(now))) {
                adopted = true;
                std::cerr << "[vc2-audio] " << kVolumes[i].name << " volume " << now << " (OPTIONS) -> settings.ini\n";
            }
        } else if (wanted >= 0 && wanted != now) {
            set_game_volume(memory, i, wanted);
            std::cerr << "[vc2-audio] " << kVolumes[i].name << " volume " << wanted << " (settings.ini; the game had "
                      << now << ")\n";
            s.last[i] = wanted;
            continue;
        }
        s.last[i] = now;
    }
    if (adopted) {
        s.options_opened = false;
        if (!options::save()) std::cerr << "[vc2-audio] the volumes were not saved to settings.ini\n";
    }
}

}  // namespace

void audio_options_opened() { state().options_opened = true; }

void install_audio(HleRegistrar &hle) {
    State &s = state();
    if (s.installed) return;
    HleFunction inner = hle.bound_function("sceDisplay", 0x289D82FEu);  // sceDisplaySetFrameBuf
    if (!inner) {
        std::cerr << "[vc2-audio] sceDisplaySetFrameBuf not bound: volumes follow the game only\n";
        return;
    }
    s.installed = true;
    hle.bind("sceDisplay", 0x289D82FEu, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
        check(rt);
        inner(rt, ctx);
    });
    std::cerr << "[vc2-audio] music " << options::get(options::Key::MusicVolume) << ", SFX "
              << options::get(options::Key::SfxVolume) << ", voice " << options::get(options::Key::VoiceVolume)
              << " (empty: the game's own)\n";
}

}  // namespace portablekit::vc2
