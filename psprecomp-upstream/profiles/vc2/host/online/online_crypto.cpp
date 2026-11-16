#include "online_crypto.hpp"

#include <sodium.h>

#include <cstring>
#include <mutex>

namespace portablekit::vc2::online {

bool crypto_ready() {
    static const bool ready = sodium_init() >= 0;
    return ready;
}

void random_bytes(void *data, std::size_t size) { randombytes_buf(data, size); }

Secret new_secret() {
    Secret secret{};
    random_bytes(secret.data(), secret.size());
    return secret;
}

std::vector<std::uint8_t> keyed_hash(const Secret &secret, const void *data, std::size_t length, std::size_t size) {
    std::vector<std::uint8_t> out(size);
    crypto_generichash(out.data(), out.size(), static_cast<const unsigned char *>(data), length, secret.data(),
                       secret.size());
    return out;
}

Key auth_key(const Secret &secret) {
    static constexpr char kLabel[] = "vc2-online auth";
    const auto hash = keyed_hash(secret, kLabel, sizeof(kLabel) - 1u);
    Key key{};
    std::memcpy(key.data(), hash.data(), key.size());
    return key;
}

void tag(const Key &key, const std::uint8_t *data, std::size_t length, std::uint8_t out[32]) {
    crypto_auth(out, data, length, key.data());
}

bool tag_ok(const Key &key, const std::uint8_t *data, std::size_t length, const std::uint8_t tag[32]) {
    return crypto_auth_verify(tag, data, length, key.data()) == 0;
}

Key new_identity() {
    Key key{};
    random_bytes(key.data(), key.size());
    return key;
}

Key identity_public(const Key &secret_key) {
    Key out{};
    crypto_scalarmult_base(out.data(), secret_key.data());
    return out;
}

std::optional<Secret> pair_secret(const Key &own_secret, const Key &own_public, const Key &their_public) {
    Key shared{};
    if (crypto_scalarmult(shared.data(), own_secret.data(), their_public.data()) != 0) return std::nullopt;
    static constexpr char kLabel[] = "vc2-online pair 1";
    std::vector<std::uint8_t> input(kLabel, kLabel + sizeof(kLabel) - 1u);
    input.insert(input.end(), shared.begin(), shared.end());
    const bool own_first = own_public < their_public;
    const Key &first = own_first ? own_public : their_public, &second = own_first ? their_public : own_public;
    input.insert(input.end(), first.begin(), first.end());
    input.insert(input.end(), second.begin(), second.end());
    const auto hash = plain_hash(input.data(), input.size(), 16u);
    sodium_memzero(shared.data(), shared.size());
    sodium_memzero(input.data(), input.size());
    Secret secret{};
    std::memcpy(secret.data(), hash.data(), secret.size());
    return secret;
}

std::vector<std::uint8_t> plain_hash(const void *data, std::size_t length, std::size_t size) {
    std::vector<std::uint8_t> out(size);
    crypto_generichash(out.data(), out.size(), static_cast<const unsigned char *>(data), length, nullptr, 0u);
    return out;
}

KeyPair::KeyPair() { crypto_kx_keypair(public_key.data(), secret_key.data()); }
KeyPair::~KeyPair() { sodium_memzero(secret_key.data(), secret_key.size()); }
TrafficKeys::~TrafficKeys() {
    sodium_memzero(send.data(), send.size());
    sodium_memzero(receive.data(), receive.size());
}

namespace {

// One traffic key: the exchange's key for that direction, the transcript and
// the direction's name, hashed under the secret.
Key mix(const Secret &secret, const Key &exchanged, const std::vector<std::uint8_t> &transcript, const char *label) {
    std::vector<std::uint8_t> input(exchanged.begin(), exchanged.end());
    const auto summary = keyed_hash(secret, transcript.data(), transcript.size());
    input.insert(input.end(), summary.begin(), summary.end());
    input.insert(input.end(), label, label + std::strlen(label));
    const auto hash = keyed_hash(secret, input.data(), input.size());
    sodium_memzero(input.data(), input.size());
    Key key{};
    std::memcpy(key.data(), hash.data(), key.size());
    return key;
}

} // namespace

std::optional<TrafficKeys> joiner_keys(const Secret &secret, const KeyPair &own, const Key &host_public,
                                       const std::vector<std::uint8_t> &transcript) {
    Key rx{}, tx{};
    if (crypto_kx_client_session_keys(rx.data(), tx.data(), own.public_key.data(), own.secret_key.data(),
                                      host_public.data()) != 0)
        return std::nullopt;
    TrafficKeys keys;
    keys.send = mix(secret, tx, transcript, "joiner to host");
    keys.receive = mix(secret, rx, transcript, "host to joiner");
    sodium_memzero(rx.data(), rx.size());
    sodium_memzero(tx.data(), tx.size());
    return keys;
}

std::optional<TrafficKeys> host_keys(const Secret &secret, const KeyPair &own, const Key &joiner_public,
                                     const std::vector<std::uint8_t> &transcript) {
    Key rx{}, tx{};
    if (crypto_kx_server_session_keys(rx.data(), tx.data(), own.public_key.data(), own.secret_key.data(),
                                      joiner_public.data()) != 0)
        return std::nullopt;
    TrafficKeys keys;
    keys.send = mix(secret, tx, transcript, "host to joiner");
    keys.receive = mix(secret, rx, transcript, "joiner to host");
    sodium_memzero(rx.data(), rx.size());
    sodium_memzero(tx.data(), tx.size());
    return keys;
}

namespace {
void make_nonce(std::uint64_t number, unsigned char nonce[24]) {
    std::memset(nonce, 0, 24);
    for (int i = 0; i < 8; ++i) nonce[i] = static_cast<unsigned char>(number >> (8 * i));
}
} // namespace

std::vector<std::uint8_t> seal(const Key &key, std::uint64_t number, const std::uint8_t *header,
                               std::size_t header_size, const std::uint8_t *plain, std::size_t plain_size) {
    unsigned char nonce[24];
    make_nonce(number, nonce);
    std::vector<std::uint8_t> out(plain_size + kSealOverhead);
    unsigned long long length = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.data(), &length, plain, plain_size, header, header_size, nullptr,
                                               nonce, key.data());
    out.resize(static_cast<std::size_t>(length));
    return out;
}

std::optional<std::vector<std::uint8_t>> open(const Key &key, std::uint64_t number, const std::uint8_t *header,
                                              std::size_t header_size, const std::uint8_t *sealed,
                                              std::size_t sealed_size) {
    if (sealed_size < kSealOverhead) return std::nullopt;
    unsigned char nonce[24];
    make_nonce(number, nonce);
    std::vector<std::uint8_t> out(sealed_size - kSealOverhead);
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(out.data(), &length, nullptr, sealed, sealed_size, header,
                                                   header_size, nonce, key.data()) != 0)
        return std::nullopt;
    out.resize(static_cast<std::size_t>(length));
    return out;
}

bool ReplayWindow::accept(std::uint64_t number) {
    constexpr std::uint64_t kSize = 64u * 16u;
    if (!any_ || number > highest_) {
        const std::uint64_t shift = any_ ? number - highest_ : kSize;
        if (shift >= kSize) {
            bits_.fill(0u);
        } else {
            // Moves every bit `shift` places up (older), word by word.
            for (std::uint64_t n = 0; n < shift; ++n) {
                for (std::size_t w = bits_.size(); w-- > 0;) {
                    bits_[w] <<= 1u;
                    if (w > 0 && (bits_[w - 1] >> 63u) != 0u) bits_[w] |= 1u;
                }
            }
        }
        highest_ = number;
        any_ = true;
        bits_[0] |= 1u;
        return true;
    }
    const std::uint64_t age = highest_ - number;
    if (age >= kSize) return false;
    std::uint64_t &word = bits_[age / 64u];
    const std::uint64_t bit = std::uint64_t{1} << (age % 64u);
    if ((word & bit) != 0u) return false;
    word |= bit;
    return true;
}

} // namespace portablekit::vc2::online
