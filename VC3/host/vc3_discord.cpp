// Discord Rich Presence: "Playing Valkyria Chronicles 3", what the player is
// doing (title screen, Base Camp, the march map, a battle) and for how long, on their
// Discord profile.
//
// It talks to the Discord app on this PC itself, through Discord's local IPC
// pipe (\\.\pipe\discord-ipc-0..9: frames of a little-endian opcode, a
// length and JSON; handshake {"v":1,"client_id":...} as opcode 0, then
// SET_ACTIVITY commands as opcode 1), so it needs no Discord SDK or library
// and ships nothing of Discord's. Nothing is sent anywhere else: without the
// Discord app running it does nothing, and it tries again every 20 s.
//
// Off with vc3.discord=off (launcher: Network > Discord status) or
// VC3_DISCORD=off. The Discord application's ID: kApplicationId below, or
// VC3_DISCORD_APP=<id> (the Discord Developer Portal's Application ID). Its
// art: an asset named "logo" in that application.
//
// What it shows comes from what the port already knows: the title menu
// (vc3_native_gu.cpp), Action and Target Mode (vc3_camera.cpp), and which of
// the game's message files its last messages by id came from (vc3_text.cpp;
// VC3's file table 0x08CBAAC8 gives each file a range of ids): Base Camp's
// (mtpa_camp), the march map's (mtpa_march) or a battle's (mtpa_slg,
// mtpa_slg_ev). VC2's academy map cursor is no guide here: VC3's battle map
// uses the same cursor (2026-11-16 log: the cursor between Action Modes).

#include "vc3_discord.hpp"
#include "vc3_camera.hpp"
#include "vc3_native_gu.hpp"
#include "vc3_region.hpp"
#include "vc3_settings.hpp"

#include "profile.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace portablekit::vc3 {
namespace {

// The Discord application "Valkyria Chronicles 3" (owner's Developer Portal,
// 2026-11-16; VC2 has its own, "Valkyria Chronicles II"). Its art: the asset
// "logo". VC3_DISCORD_APP=<id> uses another.
constexpr const char *kApplicationId = "1571895530894655571";

// Where the player was last, by the messages the game fetched (ms since the
// process started; 0: never).
enum Place { kCamp, kMarch, kBattle, kPlaces };
std::atomic<long long> &place_seen(Place p) {
    static std::atomic<long long> seen[kPlaces]{};
    return seen[p];
}
long long now_ms() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() + 1;
}

std::string json_escape(const std::string &text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\', out += c;
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

#if defined(_WIN32)
class Pipe {
public:
    ~Pipe() { close(); }
    bool open() {
        for (int i = 0; i < 10 && handle_ == INVALID_HANDLE_VALUE; ++i) {
            const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
            handle_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        return handle_ != INVALID_HANDLE_VALUE;
    }
    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    // One frame out, then the answer read (the pipe must not fill up).
    bool send(std::uint32_t opcode, const std::string &json, std::string *answer = nullptr) {
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        std::string frame(8, '\0');
        const std::uint32_t length = static_cast<std::uint32_t>(json.size());
        std::memcpy(frame.data(), &opcode, 4);
        std::memcpy(frame.data() + 4, &length, 4);
        frame += json;
        DWORD written = 0;
        if (!WriteFile(handle_, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) || written != frame.size()) {
            close();
            return false;
        }
        std::uint32_t head[2]{};
        if (!read(head, sizeof head)) return false;
        std::string body(head[1], '\0');
        if (head[1] > (1u << 20) || (head[1] != 0u && !read(body.data(), head[1]))) return false;
        if (head[0] == 2u) {  // CLOSE: Discord refused (a wrong application ID, say)
            std::cerr << "[vc3-discord] Discord closed the connection: " << body << "\n";
            close();
            return false;
        }
        if (answer != nullptr) *answer = std::move(body);
        return true;
    }

private:
    bool read(void *out, std::size_t size) {
        auto *bytes = static_cast<char *>(out);
        while (size > 0u) {
            DWORD got = 0;
            if (!ReadFile(handle_, bytes, static_cast<DWORD>(size), &got, nullptr) || got == 0u) {
                close();
                return false;
            }
            bytes += got, size -= got;
        }
        return true;
    }
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#endif

// What the player is doing, as Discord shows it.
struct Activity {
    std::string details, state;
    bool operator==(const Activity &) const = default;
};

Activity now_doing() {
    static bool title = false;
    if (title_menu_shown()) {
        // Leaving the title starts afresh: nothing from before it counts.
        title = true;
        return {"At the title screen", ""};
    }
    if (title) {
        title = false;
        for (int p = 0; p < kPlaces; ++p) place_seen(static_cast<Place>(p)).store(0);
    }
    if (action_mode() || target_mode()) place_seen(kBattle).store(now_ms());
    // The place whose messages (or, for a battle, Action and Target Mode) came last.
    Place where = kPlaces;
    long long latest = 0;
    for (int p = 0; p < kPlaces; ++p)
        if (const long long t = place_seen(static_cast<Place>(p)).load(); t > latest) latest = t, where = static_cast<Place>(p);
    if (where == kBattle) {
        if (target_mode()) return {"In battle", "Taking aim"};
        if (action_mode()) return {"In battle", "Leading a unit"};
        return {"In battle", "Planning the next move"};
    }
    if (where == kCamp) return {"At Base Camp", "With Squad 422"};
    if (where == kMarch) return {"On the march map", "With Squad 422"};
    return {"With Squad 422", ""};
}

// Discord status on or off (in-game Settings changes it while running).
std::atomic<bool> &enabled() {
    static std::atomic<bool> on{true};
    return on;
}

void run(std::string application) {
#if defined(_WIN32)
    const long long started =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const DWORD pid = GetCurrentProcessId();
    Pipe pipe;
    bool ready = false;
    Activity shown;
    bool told_missing = false;
    std::uint64_t nonce = 0;
    auto last_sent = std::chrono::steady_clock::now() - std::chrono::hours(1);
    bool cleared = true;  // nothing shown on the profile
    for (;;) {
        if (!enabled().load()) {
            // Off: clear what the profile shows (an activity left out clears
            // it), then do nothing until turned on again.
            if (ready && !cleared) {
                const std::string command = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) +
                                            "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
                if (!pipe.send(1u, command)) ready = false;
                std::cerr << "[vc3-discord] off: status cleared\n";
            }
            cleared = true;
            shown = {};
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        cleared = false;
        if (!ready) {
            if (!pipe.open()) {
                if (!told_missing) std::cerr << "[vc3-discord] Discord is not running; looking again every 20 s\n";
                told_missing = true;
                std::this_thread::sleep_for(std::chrono::seconds(20));
                continue;
            }
            std::string answer;
            ready = pipe.send(0u, "{\"v\":1,\"client_id\":\"" + json_escape(application) + "\"}", &answer);
            if (!ready) {
                std::this_thread::sleep_for(std::chrono::seconds(20));
                continue;
            }
            std::cerr << "[vc3-discord] connected to Discord\n";
            told_missing = false;
            shown = {};
            last_sent = std::chrono::steady_clock::now() - std::chrono::hours(1);
        }
        const Activity doing = now_doing();
        const auto now = std::chrono::steady_clock::now();
        // Discord takes about one update every 15 s; a change waits for that.
        if (!(doing == shown) && now - last_sent >= std::chrono::seconds(15)) {
            std::string activity = "{\"details\":\"" + json_escape(doing.details) + "\"";
            if (!doing.state.empty()) activity += ",\"state\":\"" + json_escape(doing.state) + "\"";
            activity += ",\"timestamps\":{\"start\":" + std::to_string(started) + "}";
            activity += ",\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Valkyria Chronicles 3\"}}";
            const std::string command = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) +
                                        ",\"activity\":" + activity + "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
            if (pipe.send(1u, command)) {
                shown = doing;
                last_sent = now;
                static std::uint32_t logged = 0u;
                if (++logged <= 50u)
                    std::cerr << "[vc3-discord] " << doing.details << (doing.state.empty() ? "" : ", ") << doing.state << "\n";
            } else {
                ready = false;
                std::cerr << "[vc3-discord] lost Discord; looking again\n";
                continue;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
#else
    (void)application;
#endif
}

}  // namespace

// VC3's message files by their ids (as the extracted files hold them; the file
// table at 0x08CBAAC8 gives each a range).
void discord_note_message(std::uint32_t id) {
    Place p = kPlaces;
    if (id >= 701500000u && id <= 701999999u) p = kCamp;             // mtpa_camp
    else if (id >= 701000000u && id <= 701499999u) p = kMarch;       // mtpa_march
    else if (id >= 703000000u && id <= 703999999u) p = kBattle;      // mtpa_slg
    else if (id >= 800000000u && id <= 899999999u) p = kBattle;      // mtpa_slg_ev
    if (p != kPlaces) place_seen(p).store(now_ms());
}

bool in_battle() {
    if (action_mode() || target_mode()) place_seen(kBattle).store(now_ms());
    const long long battle = place_seen(kBattle).load();
    return battle != 0 && battle >= place_seen(kCamp).load() && battle >= place_seen(kMarch).load();
}

void set_discord(bool on) {
    if (enabled().exchange(on) != on) std::cerr << "[vc3-discord] Discord status " << (on ? "on" : "off") << "\n";
}

void start_discord() {
    const std::string &choice = options::get(options::Key::Discord);
    enabled().store(choice != "off");
    if (choice == "off") std::cerr << "[vc3-discord] off (Discord status); the in-game Settings can turn it on\n";
    std::string application = kApplicationId;
    if (const char *given = env("DISCORD_APP"); given != nullptr && *given != '\0') application = given;
    if (application.empty()) {
        std::cerr << "[vc3-discord] no Discord application ID yet (VC3_DISCORD_APP); off\n";
        return;
    }
    std::thread(run, application).detach();
}

}  // namespace portablekit::vc3
