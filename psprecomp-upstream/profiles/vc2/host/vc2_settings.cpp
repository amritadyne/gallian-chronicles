#include "vc2_settings.hpp"

#include "profile.hpp"
#include "install/user_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace portablekit::vc2::options {
namespace {

constexpr std::size_t kCount = static_cast<std::size_t>(Key::Count);
struct Field { const char *key; const char *variable; const char *fallback; };
constexpr std::array<Field, kCount> kFields{{
    {"vc2.tick_hz", "TICK_HZ", "0"},
    {"vc2.lod", "LOD", "original"},
    {"vc2.controls", "CONTROLS", "modern"},
    {"vc2.stick_dead_zone", "STICK_DEAD_ZONE", "original"},
    {"vc2.prompts", "PROMPTS", "auto"},
    {"vc2.discord", "DISCORD", "on"},
    // Full-screen pictures in a window wider (or taller) than the PSP's shape:
    // "original" (at the PSP's shape, black beside them), "zoom" (zoomed to
    // fill the window, cropped above and below; movies stay whole), "zoom-movies"
    // (movies too). vc2_native_gu.cpp.
    {"vc2.backgrounds", "BACKGROUNDS", "original"},
    // The game's own OPTIONS volumes, 0..10 (vc2_audio.cpp). Empty (the key
    // not in settings.ini): the game keeps its own, from its save.
    {"vc2.music_volume", "MUSIC_VOLUME", ""},
    {"vc2.sfx_volume", "SFX_VOLUME", ""},
    {"vc2.voice_volume", "VOICE_VOLUME", ""},
    // Online play (vc2_online.cpp). The code is a password: never logged.
    {"vc2.online", "ONLINE", "off"},
    {"vc2.online_code", "ONLINE_CODE", ""},
    {"vc2.online_port", "ONLINE_PORT", "27320"},
    {"vc2.online_router", "ONLINE_ROUTER", "on"},
    {"vc2.online_stun", "ONLINE_STUN", "on"},
    {"vc2.online_key", "ONLINE_KEY", ""},
    {"vc2.online_my_code", "ONLINE_MY_CODE", ""},
    {"vc2.online_friend1", "ONLINE_FRIEND1", ""},
    {"vc2.online_friend2", "ONLINE_FRIEND2", ""},
    {"vc2.online_friend3", "ONLINE_FRIEND3", ""},
    // Voices from another release's disc image (vc2_voice.cpp).
    {"vc2.voice_disc", "VOICE_DISC", ""},
    {"vc2.voice_replace", "VOICE_REPLACE", "off"},
}};

struct State {
    std::filesystem::path directory;
    std::array<std::string, kCount> values;
    std::array<bool, kCount> overrides{};
    std::array<bool, kCount> changed{};
};

bool number_in(std::string_view text, double low, double high) {
    const std::string owned(text);
    char *end = nullptr;
    const double number = std::strtod(owned.c_str(), &end);
    return end != owned.c_str() && end == owned.c_str() + owned.size() &&
           std::isfinite(number) && number >= low && number <= high;
}

bool valid(Key key, std::string_view value) {
    switch (key) {
    case Key::TickHz: return value == "0" || value == "uncapped" || number_in(value, 30, 1000);
    case Key::Lod: return value == "original" || value == "full";
    case Key::Controls: return value == "modern" || value == "classic";
    case Key::StickDeadZone:
        return value == "original" || value == "square" || number_in(value, 0, 40);
    case Key::Prompts:
        return value == "auto" || value == "playstation" || value == "xbox" || value == "keyboard";
    case Key::Discord: return value == "on" || value == "off";
    case Key::Backgrounds: return value == "original" || value == "zoom" || value == "zoom-movies";
    case Key::MusicVolume:
    case Key::SfxVolume:
    case Key::VoiceVolume:
        return !value.empty() && value.size() <= 2u && value.find_first_not_of("0123456789") == std::string_view::npos &&
               number_in(value, 0, 10);
    case Key::Online: return value == "off" || value == "on" || value == "host" || value == "join";
    case Key::OnlineKey:
        return value.empty() || (value.size() == 64u && value.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos);
    case Key::OnlineMyCode:
    case Key::OnlineFriend1:
    case Key::OnlineFriend2:
    case Key::OnlineFriend3:
    case Key::OnlineCode:
        return value.size() <= 200u &&
               value.find_first_not_of("0123456789ABCDEFGHJKMNPQRSTVWXYZabcdefghjkmnpqrstvwxyzOoIiLl- ") ==
                   std::string_view::npos;
    case Key::OnlinePort:
        return !value.empty() && value.find_first_not_of("0123456789") == std::string_view::npos &&
               number_in(value, 1024, 65000);
    case Key::OnlineRouter:
    case Key::OnlineStun:
    case Key::VoiceReplace: return value == "on" || value == "off";
    case Key::VoiceDisc:
        return value.size() <= 1024u &&
               std::find_if(value.begin(), value.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20u; }) ==
                   value.end();
    default: return false;
    }
}

State &state() {
    static State s = [] {
        State initial;
        install::SettingsEntries entries;
        try {
            initial.directory = install::user_data_directory();
            entries = install::read_settings_file(initial.directory);
        } catch (const std::exception &e) {
            std::cerr << "[vc2-settings] cannot read settings.ini: " << e.what() << "\n";
        }
        for (std::size_t i = 0; i < kCount; ++i) {
            const auto &field = kFields[i];
            auto &value = initial.values[i];
            value = field.fallback;
            if (const auto found = entries.find(field.key); found != entries.end()) {
                if (valid(static_cast<Key>(i), found->second)) value = found->second;
                else std::cerr << "[vc2-settings] invalid " << field.key << "; using " << field.fallback << "\n";
            }
            if (const char *override_value = env(field.variable)) {
                value = override_value;
                initial.overrides[i] = true;
            }
            // These diagnostic switches also control the effective setting.
            if (i == static_cast<std::size_t>(Key::Lod) && env("LOD_SCALE")) initial.overrides[i] = true;
            if (i == static_cast<std::size_t>(Key::Prompts) && env("NO_PROMPTS")) initial.overrides[i] = true;
            const bool secret = (i == static_cast<std::size_t>(Key::OnlineCode) ||
                                 i == static_cast<std::size_t>(Key::OnlineKey)) && !value.empty();
            std::cerr << "[vc2-settings] " << field.key << "=" << (secret ? "(set)" : value)
                      << (initial.overrides[i] ? " (environment override)" : " (settings.ini/default)") << "\n";
        }
        return initial;
    }();
    return s;
}

std::size_t index(Key key) {
    const auto i = static_cast<std::size_t>(key);
    if (i >= kCount) throw std::out_of_range("VC2 setting key");
    return i;
}

} // namespace

void load() { (void)state(); }
const std::string &get(Key key) { return state().values[index(key)]; }
bool overridden(Key key) { return state().overrides[index(key)]; }

bool set(Key key, std::string_view value) {
    auto &s = state();
    const auto i = index(key);
    if (s.overrides[i] || !valid(key, value)) return false;
    if (s.values[i] != value) {
        s.values[i] = value;
        s.changed[i] = true;
    }
    return true;
}

bool dirty() {
    for (bool changed : state().changed) if (changed) return true;
    return false;
}

bool save() {
    auto &s = state();
    if (!dirty()) return true;
    try {
        if (s.directory.empty()) throw std::runtime_error("no data directory");
        auto entries = install::read_settings_file(s.directory);
        for (std::size_t i = 0; i < kCount; ++i)
            if (s.changed[i] && !s.overrides[i]) entries[kFields[i].key] = s.values[i];
        install::write_settings_file(s.directory, entries);
        s.changed.fill(false);
        std::cerr << "[vc2-settings] saved Options changes\n";
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[vc2-settings] cannot save Options changes: " << e.what() << "\n";
        return false;
    }
}

} // namespace portablekit::vc2::options
