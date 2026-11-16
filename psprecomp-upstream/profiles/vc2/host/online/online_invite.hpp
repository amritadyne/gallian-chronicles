#pragma once

// Invite codes and reply codes.
//
// Invite code: "VC2-" then Crockford base32 in groups of five, of
//   version (1) | the session secret (16) | counts (1: IPv4 in the low four
//   bits, IPv6 in the high four) | each IPv4 address and port (6) | each
//   IPv6 address and port (18) | check (2: keyed hash of the rest under the
//   secret, so a mistyped code is caught).
// Anyone holding it can join, so it is shared like a password, and it dies
// when the host stops hosting (a new secret each time).
//
// Reply code (the joiner's answer when the host cannot be reached directly):
// "VC2R-" then base32 of a random 8-byte nonce and the joiner's addresses,
// sealed under a key derived from the secret: only the host can read it, and
// only a joiner holding the invite can make one.
#include "online_crypto.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace portablekit::vc2::online {

struct Endpoint {
    bool v6{};
    std::array<std::uint8_t, 16> ip{};  // IPv4 in the first four bytes
    std::uint16_t port{};

    [[nodiscard]] std::string describe() const;
    friend bool operator==(const Endpoint &a, const Endpoint &b) {
        return a.v6 == b.v6 && a.port == b.port &&
               (a.v6 ? a.ip == b.ip : std::equal(a.ip.begin(), a.ip.begin() + 4, b.ip.begin()));
    }
};

[[nodiscard]] Endpoint endpoint_v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d,
                                   std::uint16_t port);

constexpr std::size_t kMaxV4Candidates = 4u;
constexpr std::size_t kMaxV6Candidates = 2u;

struct Invite {
    Secret secret{};
    std::vector<Endpoint> candidates;  // the order to try, the most likely first
};

[[nodiscard]] std::string encode_invite(const Invite &invite);
// Spaces, dashes and case do not matter; O reads as 0, I and L as 1.
[[nodiscard]] std::optional<Invite> decode_invite(const std::string &text);

[[nodiscard]] std::string encode_reply(const Secret &secret, const std::vector<Endpoint> &joiner);
[[nodiscard]] std::optional<std::vector<Endpoint>> decode_reply(const Secret &secret, const std::string &text);

// Friend code (each player's own, swapped once in the launcher):
// "VC2F-" then base32 of version (1) | the player's public key (32) | counts
// and addresses as in an invite | check (2: unkeyed hash of the rest). It is
// not a password: a session forms only between players who have each other's
// codes (online_crypto.hpp, pair_secret). The addresses are the player's when
// it was made, so it is made again when they change.
struct FriendCode {
    Key public_key{};
    std::vector<Endpoint> candidates;
};
[[nodiscard]] std::string encode_friend(const FriendCode &code);
[[nodiscard]] std::optional<FriendCode> decode_friend(const std::string &text);
[[nodiscard]] bool looks_like_friend(const std::string &text);

// Whether text looks like either kind of code (for a paste box).
[[nodiscard]] bool looks_like_invite(const std::string &text);
[[nodiscard]] bool looks_like_reply(const std::string &text);

// Crockford base32, for the tests.
[[nodiscard]] std::string base32_encode(const std::vector<std::uint8_t> &bytes);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base32_decode(const std::string &text);

} // namespace portablekit::vc2::online
