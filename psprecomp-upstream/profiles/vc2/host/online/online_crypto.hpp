#pragma once

// The online session's cryptography, on libsodium (online_session.cpp says how
// it is used).
//
// Everything rests on the invite code's 16-byte secret, the pre-shared key:
//   - HELLO and WELCOME carry an HMAC-SHA-512-256 tag under a key derived from
//     the secret, so only a holder of the code can start a session, and the
//     host answers nobody else;
//   - each side makes a fresh X25519 key pair per session (crypto_kx), and the
//     two traffic keys mix that exchange with the secret and the transcript of
//     both messages: forward secrecy from the key pairs, authentication from
//     the secret (the shape of Noise NNpsk0);
//   - every packet after that is XChaCha20-Poly1305 with its 64-bit packet
//     number as the nonce, one key per direction, and its header as
//     associated data; a 1024-packet window refuses replays.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace portablekit::vc2::online {

using Secret = std::array<std::uint8_t, 16>;
using Key = std::array<std::uint8_t, 32>;

// sodium_init(), once. False when libsodium cannot start (no session then).
[[nodiscard]] bool crypto_ready();

void random_bytes(void *data, std::size_t size);
[[nodiscard]] Secret new_secret();

// generichash (BLAKE2b) of `data` keyed with the secret, `size` bytes.
[[nodiscard]] std::vector<std::uint8_t> keyed_hash(const Secret &secret, const void *data, std::size_t length,
                                                   std::size_t size = 32u);

// The handshake's tag key.
[[nodiscard]] Key auth_key(const Secret &secret);
void tag(const Key &key, const std::uint8_t *data, std::size_t length, std::uint8_t out[32]);
[[nodiscard]] bool tag_ok(const Key &key, const std::uint8_t *data, std::size_t length, const std::uint8_t tag[32]);

// Player identities (friend codes, online_invite.hpp): an X25519 key pair kept
// in the settings. Two friends' sessions rest on a secret only the two of them
// can make, from one's secret key and the other's public key (the
// static-static exchange, hashed with both public keys): a session forms only
// between players who have each other's codes.
[[nodiscard]] Key new_identity();                     // a new secret key
[[nodiscard]] Key identity_public(const Key &secret_key);
[[nodiscard]] std::optional<Secret> pair_secret(const Key &own_secret, const Key &own_public, const Key &their_public);
// Unkeyed generichash, `size` bytes (friend codes' check).
[[nodiscard]] std::vector<std::uint8_t> plain_hash(const void *data, std::size_t length, std::size_t size);

struct KeyPair {
    Key public_key{};
    Key secret_key{};
    KeyPair();   // a new one
    ~KeyPair();  // wipes the secret key
    KeyPair(const KeyPair &) = default;
    KeyPair &operator=(const KeyPair &) = default;
};

// The two traffic keys of a session.
struct TrafficKeys {
    Key send{};
    Key receive{};
    ~TrafficKeys();
};

// `transcript` is the HELLO followed by the WELCOME without its tag.
[[nodiscard]] std::optional<TrafficKeys> joiner_keys(const Secret &secret, const KeyPair &own, const Key &host_public,
                                                     const std::vector<std::uint8_t> &transcript);
[[nodiscard]] std::optional<TrafficKeys> host_keys(const Secret &secret, const KeyPair &own,
                                                   const Key &joiner_public,
                                                   const std::vector<std::uint8_t> &transcript);

constexpr std::size_t kSealOverhead = 16u;

// Packet sealing: `header` is authenticated, `plain` encrypted; the nonce is
// the packet number.
[[nodiscard]] std::vector<std::uint8_t> seal(const Key &key, std::uint64_t number, const std::uint8_t *header,
                                             std::size_t header_size, const std::uint8_t *plain,
                                             std::size_t plain_size);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> open(const Key &key, std::uint64_t number,
                                                            const std::uint8_t *header, std::size_t header_size,
                                                            const std::uint8_t *sealed, std::size_t sealed_size);

// Refuses packet numbers seen before, or older than the last 1024.
class ReplayWindow {
public:
    // True the first time a number is offered (and it is not too old). Only
    // offer numbers whose packet authenticated.
    bool accept(std::uint64_t number);

private:
    std::uint64_t highest_{};
    bool any_{};
    std::array<std::uint64_t, 16> bits_{};  // bit i: highest_ - i seen
};

} // namespace portablekit::vc2::online
