// vc2-online-code: makes this player's friend code for the launcher (Network
// page, "Your code"). Online play's own code (host/online), run without the
// game: the launcher starts it hidden and reads its answer.
//
//   vc2-online-code [--key HEX] [--port 27320] [--router on|off] [--stun on|off]
//
// Without --key a new identity is made. Prints, one per line:
//   KEY <the secret key, 64 hex: the launcher keeps it in settings.ini>
//   CODE <the friend code, VC2F-...>
//   ROUTER mapped|no     (the router opened the port when asked: UPnP/NAT-PMP)
//   PUBLIC <address>|none (the public address STUN saw)
// or ERROR <why>. What it did goes to stderr.
#include "online_crypto.hpp"
#include "online_invite.hpp"
#include "online_nat.hpp"
#include "online_session.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

using namespace portablekit::vc2::online;

namespace {

std::optional<Key> from_hex(const std::string &hex) {
    if (hex.size() != 64u) return std::nullopt;
    Key key{};
    for (std::size_t i = 0; i < 32u; ++i) {
        unsigned value = 0;
        if (std::sscanf(hex.c_str() + 2 * i, "%2x", &value) != 1) return std::nullopt;
        key[i] = static_cast<std::uint8_t>(value);
    }
    return key;
}

std::string to_hex(const Key &key) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : key) {
        out += kHex[b >> 4];
        out += kHex[b & 15u];
    }
    return out;
}

int fail(const std::string &why) {
    std::cout << "ERROR " << why << std::endl;
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    std::string key_hex, router = "on", stun = "on";
    int port = 27320;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string name = argv[i], value = argv[i + 1];
        if (name == "--key") key_hex = value;
        else if (name == "--port") port = std::atoi(value.c_str());
        else if (name == "--router") router = value;
        else if (name == "--stun") stun = value;
    }
    if (port < 1024 || port > 65000) return fail("the port must be 1024 to 65000");
    if (!crypto_ready()) return fail("the encryption library did not start");
    Key key{};
    if (key_hex.empty()) {
        key = new_identity();
    } else if (const auto given = from_hex(key_hex)) {
        key = *given;
    } else {
        return fail("the stored key is not valid");
    }
    // A session on the online port, only for its socket: STUN must see the
    // address the game will use.
    Session session;
    SessionConfig config;
    config.role = Role::Host;
    config.secret = new_secret();
    config.edition = "code";
    config.udp_port = static_cast<std::uint16_t>(port);
    if (!session.start(config)) return fail("no UDP port: " + session.status().error);
    if (session.status().udp_port != port) {
        session.stop();
        return fail("UDP port " + std::to_string(port) + " is in use (is the game running? close it and try again)");
    }
    std::optional<Mapping> mapping;
    std::string log;
    const auto found = gather_addresses(session, static_cast<std::uint16_t>(port), router == "on", stun == "on",
                                        mapping, log);
    std::cerr << log;
    if (mapping) unmap_port(*mapping);  // the game maps it again while it runs
    session.stop();
    FriendCode code;
    code.public_key = identity_public(key);
    code.candidates = found;
    std::string public_address = "none";
    for (const Endpoint &e : found)
        if (!e.v6 && !is_private_v4(e)) {
            public_address = e.describe();
            break;
        }
    std::cout << "KEY " << to_hex(key) << "\n"
              << "CODE " << encode_friend(code) << "\n"
              << "ROUTER " << (mapping ? "mapped" : "no") << "\n"
              << "PUBLIC " << public_address << std::endl;
    return 0;
}
