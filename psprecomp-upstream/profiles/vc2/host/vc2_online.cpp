#include "vc2_online.hpp"

#include "online/online_invite.hpp"
#include "online/online_nat.hpp"
#include "online/online_session.hpp"
#include "vc2_region.hpp"
#include "vc2_settings.hpp"

#include "adhoc/session.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace portablekit::vc2 {
namespace {

using namespace online;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr auto kPunchAgain = milliseconds(15000);

struct Friend {
    int slot{};  // 1..3, the launcher's box
    FriendCode code;
    Secret secret{};
};

struct State {
    std::mutex mutex;
    std::condition_variable wake;
    bool active{};
    Role role{Role::Host};
    Session session;
    std::thread helper;
    std::atomic<bool> quit{false};
    std::optional<Mapping> mapping;
    bool adhoc_ready{};
    Key public_key{};
    std::vector<Friend> friends;
    int hub_slot{-1};          // joiner: the friend whose PC runs the session
    std::optional<FriendCode> own_code;  // as the launcher made it
    bool code_stale{};
};

State &state() {
    static State *s = new State;  // lives to the end: atexit uses it
    return *s;
}

std::string edition() { return region() == Region::Japan ? "vc2-jp-101" : "vc2-west-1"; }

// The window title ------------------------------------------------------------
// The only thing online play shows in the game: its state after the title.

#if defined(_WIN32)
BOOL CALLBACK find_own_window(HWND window, LPARAM found) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
        return TRUE;
    *reinterpret_cast<HWND *>(found) = window;
    return FALSE;
}

void show_status(const std::string &status) {
    static HWND window = nullptr;
    static std::wstring original;
    static std::string shown;
    if (window == nullptr || !IsWindow(window)) {
        window = nullptr;
        EnumWindows(find_own_window, reinterpret_cast<LPARAM>(&window));
        if (window == nullptr) return;
        wchar_t title[256] = {};
        GetWindowTextW(window, title, 256);
        original = title;
        shown.clear();
    }
    if (status == shown) return;
    shown = status;
    const std::string text = status.empty() ? std::string() : "  ·  Online: " + status;
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size > 0 ? size : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    wide.resize(wcslen(wide.c_str()));
    // WM_SETTEXT to the window's own thread, with a time limit (so a game
    // that is closing, waiting for this helper, is never held up); the text
    // stays alive in a ring, as a message that timed out may still be read.
    static std::wstring ring[8];
    static unsigned next = 0;
    std::wstring &line = ring[next++ % 8u];
    line = original + wide;
    DWORD_PTR result = 0;
    SendMessageTimeoutW(window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(line.c_str()), SMTO_ABORTIFHUNG, 250, &result);
}
#else
void show_status(const std::string &status) {
    static std::string shown;
    if (status != shown) std::cerr << "[vc2-online] status: " << (shown = status) << "\n";
}
#endif

// Codes -----------------------------------------------------------------------

std::optional<Key> key_from_hex(const std::string &hex) {
    if (hex.size() != 64u) return std::nullopt;
    Key key{};
    for (std::size_t i = 0; i < 32u; ++i) {
        const auto nibble = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        };
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        key[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return key;
}

// The launcher's code is out of date when the internet sees this PC at a
// public address it does not hold.
void check_own_code(const std::vector<Endpoint> &now) {
    State &s = state();
    bool stale = !s.own_code || s.own_code->public_key != s.public_key;
    if (!stale) {
        for (const Endpoint &e : now) {
            if (e.v6 || is_private_v4(e)) continue;
            const auto &held = s.own_code->candidates;
            const bool known = std::any_of(held.begin(), held.end(), [&](const Endpoint &h) {
                return !h.v6 && std::equal(h.ip.begin(), h.ip.begin() + 4, e.ip.begin());
            });
            if (!known) stale = true;
        }
    }
    if (stale)
        std::cerr << "[vc2-online] this PC's code in the launcher is out of date (its public address changed): "
                     "make a new one and send it to your friends\n";
    std::lock_guard lock(s.mutex);
    s.code_stale = stale;
}

std::string ping_of(const PeerInfo &peer) {
    return peer.round_trip_ms >= 0 ? " (" + std::to_string(peer.round_trip_ms) + " ms)" : "";
}

std::string with_stale(const std::string &line) {
    State &s = state();
    std::lock_guard lock(s.mutex);
    return s.code_stale ? line + "  ·  your code is out of date, make a new one in the launcher" : line;
}

// The helper threads ----------------------------------------------------------

std::vector<Endpoint> gather(std::uint16_t port) {
    State &s = state();
    std::optional<Mapping> mapping;
    std::string log;
    const auto found = gather_addresses(s.session, port, options::get(options::Key::OnlineRouter) == "on",
                                        options::get(options::Key::OnlineStun) == "on", mapping, log);
    std::cerr << log;
    {
        std::lock_guard lock(s.mutex);
        s.mapping = mapping;
    }
    check_own_code(found);
    return found;
}

// This PC runs the session: the friends connect to it. Each friend's code says
// where they are, so both sides open the way at once: their HELLOs out, our
// PUNCHes to them (until they are connected).
void hub_helper() {
    State &s = state();
    const std::uint16_t port = s.session.status().udp_port;
    show_status("starting");
    gather(port);
    Clock::time_point last_punch{};
    while (!s.quit) {
        const SessionStatus status = s.session.status();
        std::vector<Endpoint> waiting;
        std::string connected;
        int count = 0;
        for (const Friend &f : s.friends) {
            const auto peer = std::find_if(status.peers.begin(), status.peers.end(),
                                           [&](const PeerInfo &p) { return p.slot == f.slot; });
            if (peer == status.peers.end()) {
                waiting.insert(waiting.end(), f.code.candidates.begin(), f.code.candidates.end());
            } else {
                ++count;
                connected += (connected.empty() ? "" : ", ") + std::string("friend ") + std::to_string(f.slot) + ping_of(*peer);
            }
        }
        if (!waiting.empty() && Clock::now() - last_punch >= kPunchAgain) {
            last_punch = Clock::now();
            s.session.punch(waiting);
        }
        const std::string of = std::to_string(count) + " of " + std::to_string(s.friends.size()) + " friends connected";
        show_status(with_stale(count == 0 ? "waiting for your friends (this PC runs the session)"
                                          : of + ": " + connected));
        std::unique_lock lock(s.mutex);
        s.wake.wait_for(lock, milliseconds(500), [&] { return s.quit.load(); });
    }
}

// A friend's PC runs the session (the first code of the group): connect to it.
void join_helper() {
    State &s = state();
    const std::uint16_t port = s.session.status().udp_port;
    show_status("connecting to friend " + std::to_string(s.hub_slot));
    gather(port);
    while (!s.quit) {
        const SessionStatus status = s.session.status();
        if (status.connected && !status.peers.empty())
            show_status(with_stale("connected to friend " + std::to_string(s.hub_slot) + ping_of(status.peers[0])));
        else if (!status.error.empty())
            show_status(with_stale("friend " + std::to_string(s.hub_slot) + " refused: " + status.error));
        else
            show_status(with_stale("waiting for friend " + std::to_string(s.hub_slot) +
                                   " (their PC runs the session; start their game too)"));
        std::unique_lock lock(s.mutex);
        s.wake.wait_for(lock, milliseconds(500), [&] { return s.quit.load(); });
    }
}

void shutdown_online() {
    State &s = state();
    if (!s.active) return;
    s.quit = true;
    s.wake.notify_all();
    if (s.helper.joinable()) s.helper.join();
    s.session.stop();
    if (s.mapping) unmap_port(*s.mapping);
    s.active = false;
}

} // namespace

bool online_active() { return state().active; }

void install_online() {
    State &s = state();
    if (s.active) return;
    if (options::get(options::Key::Online) == "off") return;
    if (!crypto_ready()) {
        std::cerr << "[vc2-online] the encryption library did not start: online play is off\n";
        return;
    }
    const auto key = key_from_hex(options::get(options::Key::OnlineKey));
    if (!key) {
        std::cerr << "[vc2-online] no player code yet (launcher, Network: Your code): online play is off\n";
        show_status("off, no code yet (make one in the launcher)");
        return;
    }
    s.public_key = identity_public(*key);
    s.own_code = decode_friend(options::get(options::Key::OnlineMyCode));
    const options::Key boxes[] = {options::Key::OnlineFriend1, options::Key::OnlineFriend2, options::Key::OnlineFriend3};
    for (int slot = 1; slot <= 3; ++slot) {
        const std::string &text = options::get(boxes[slot - 1]);
        if (text.empty()) continue;
        const auto code = decode_friend(text);
        if (!code) {
            std::cerr << "[vc2-online] friend " << slot << ": the code is not valid (copy it again)\n";
            continue;
        }
        if (code->public_key == s.public_key) {
            std::cerr << "[vc2-online] friend " << slot << " holds this PC's own code: skipped\n";
            continue;
        }
        const auto secret = pair_secret(*key, s.public_key, code->public_key);
        if (!secret) continue;
        Friend f;
        f.slot = slot;
        f.code = *code;
        f.secret = *secret;
        s.friends.push_back(f);
        std::cerr << "[vc2-online] friend " << slot << ": " << code->candidates.size() << " address(es)\n";
    }
    if (s.friends.empty()) {
        std::cerr << "[vc2-online] no friends' codes (launcher, Network): online play is off\n";
        show_status("off, no friends' codes in the launcher");
        return;
    }
    // The session runs on the PC whose code sorts first: every player, holding
    // the same codes, picks the same one without asking.
    const Friend *first = nullptr;
    for (const Friend &f : s.friends)
        if (first == nullptr || f.code.public_key < first->code.public_key) first = &f;
    SessionConfig config;
    config.edition = edition();
    config.udp_port = static_cast<std::uint16_t>(std::atoi(options::get(options::Key::OnlinePort).c_str()));
    if (s.public_key < first->code.public_key) {
        s.role = Role::Host;
        config.role = Role::Host;
        config.adhocctl_port = static_cast<std::uint16_t>(settings::current().adhoc_host_port);
        for (const Friend &f : s.friends) {
            config.secrets.push_back(f.secret);
            config.slots.push_back(f.slot);
        }
        config.secret = config.secrets.front();
        std::cerr << "[vc2-online] this PC runs the session for " << s.friends.size() << " friend(s)\n";
    } else {
        s.role = Role::Join;
        s.hub_slot = first->slot;
        config.role = Role::Join;
        config.secret = first->secret;
        config.host_slot = first->slot;
        config.candidates = first->code.candidates;
        std::cerr << "[vc2-online] friend " << first->slot << "'s PC runs the session; connecting:";
        for (const Endpoint &e : config.candidates) std::cerr << " " << e.describe();
        std::cerr << "\n";
    }
    s.active = true;
    if (!s.session.start(config)) {
        show_status("cannot start: " + s.session.status().error);
        return;
    }
    s.quit = false;
    s.helper = std::thread(s.role == Role::Host ? hub_helper : join_helper);
    std::atexit(shutdown_online);
}

// Called when the game goes on line (sceNetAdhocctlInit): the session PC's
// built-in server, private to it; the others' games pointed at the session.
bool online_prepare_adhoc() {
    State &s = state();
    if (!s.active) return false;
    {
        std::lock_guard lock(s.mutex);
        if (s.adhoc_ready) return true;
        s.adhoc_ready = true;
    }
    settings::current().adhoc = true;
    if (s.role == Role::Host) {
        adhoc_host_set_private(true);
        if (!adhoc_hosting() && !adhoc_host_start())
            std::cerr << "[vc2-online] the built-in server did not start: " << adhoc_host_error() << "\n";
    } else {
        const std::uint16_t port = s.session.status().local_adhocctl_port;
        if (port != 0u) {
            adhoc_set_session_server("127.0.0.1:" + std::to_string(port));
            adhoc_apply_settings(true);
        }
    }
    return true;
}

} // namespace portablekit::vc2
