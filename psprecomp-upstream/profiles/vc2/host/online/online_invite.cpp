#include "online_invite.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace portablekit::vc2::online {
namespace {

constexpr std::uint8_t kInviteVersion = 1u;
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr char kInvitePrefix[] = "VC2-";
constexpr char kReplyPrefix[] = "VC2R-";
constexpr char kFriendPrefix[] = "VC2F-";
constexpr std::uint8_t kFriendVersion = 1u;

int digit_value(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    for (int i = 0; i < 32; ++i)
        if (kAlphabet[i] == c) return i;
    return -1;
}

std::string grouped(const std::string &prefix, const std::string &digits) {
    std::string out = prefix;
    for (std::size_t i = 0; i < digits.size(); i += 5u) {
        if (i != 0u) out += '-';
        out += digits.substr(i, 5u);
    }
    return out;
}

// The part after the prefix, or nullopt when the prefix is not there.
std::optional<std::string> body_after(const std::string &text, const char *prefix) {
    std::string compact;
    for (char c : text)
        if (std::isalnum(static_cast<unsigned char>(c))) compact += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::string want;
    for (const char *p = prefix; *p != 0; ++p)
        if (std::isalnum(static_cast<unsigned char>(*p))) want += *p;
    if (compact.compare(0, want.size(), want) != 0) return std::nullopt;
    return compact.substr(want.size());
}

void put_endpoint(std::vector<std::uint8_t> &out, const Endpoint &e) {
    out.insert(out.end(), e.ip.begin(), e.ip.begin() + (e.v6 ? 16 : 4));
    out.push_back(static_cast<std::uint8_t>(e.port >> 8));
    out.push_back(static_cast<std::uint8_t>(e.port));
}

// Writes the counts byte and the endpoints (at most the limits).
void put_endpoints(std::vector<std::uint8_t> &out, const std::vector<Endpoint> &all) {
    std::vector<Endpoint> v4, v6;
    for (const Endpoint &e : all) {
        auto &list = e.v6 ? v6 : v4;
        if (list.size() < (e.v6 ? kMaxV6Candidates : kMaxV4Candidates) &&
            std::find(list.begin(), list.end(), e) == list.end())
            list.push_back(e);
    }
    out.push_back(static_cast<std::uint8_t>(v4.size() | (v6.size() << 4u)));
    for (const Endpoint &e : v4) put_endpoint(out, e);
    for (const Endpoint &e : v6) put_endpoint(out, e);
}

// Reads them back from `at`; false when the bytes do not fit.
bool get_endpoints(const std::vector<std::uint8_t> &in, std::size_t &at, std::size_t end,
                   std::vector<Endpoint> &out) {
    if (at >= end) return false;
    const std::size_t v4 = in[at] & 0x0Fu, v6 = in[at] >> 4u;
    ++at;
    if (v4 > kMaxV4Candidates || v6 > kMaxV6Candidates || at + v4 * 6u + v6 * 18u != end) return false;
    for (std::size_t i = 0; i < v4 + v6; ++i) {
        Endpoint e;
        e.v6 = i >= v4;
        const std::size_t size = e.v6 ? 16u : 4u;
        std::copy(in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + size),
                  e.ip.begin());
        at += size;
        e.port = static_cast<std::uint16_t>((in[at] << 8) | in[at + 1]);
        at += 2u;
        if (e.port == 0u) return false;
        out.push_back(e);
    }
    return true;
}

Key reply_key(const Secret &secret) {
    static constexpr char kLabel[] = "vc2-online reply";
    const auto hash = keyed_hash(secret, kLabel, sizeof(kLabel) - 1u);
    Key key{};
    std::memcpy(key.data(), hash.data(), key.size());
    return key;
}

} // namespace

std::string Endpoint::describe() const {
    std::string text;
    if (!v6) {
        text = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
               std::to_string(ip[3]);
        return text + ":" + std::to_string(port);
    }
    static constexpr char kHex[] = "0123456789abcdef";
    text = "[";
    for (int i = 0; i < 16; i += 2) {
        if (i != 0) text += ':';
        const unsigned value = (ip[i] << 8) | ip[i + 1];
        bool started = false;
        for (int shift = 12; shift >= 0; shift -= 4) {
            const unsigned nibble = (value >> shift) & 0xFu;
            if (nibble != 0u || started || shift == 0) {
                text += kHex[nibble];
                started = true;
            }
        }
    }
    return text + "]:" + std::to_string(port);
}

Endpoint endpoint_v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port) {
    Endpoint e;
    e.ip[0] = a;
    e.ip[1] = b;
    e.ip[2] = c;
    e.ip[3] = d;
    e.port = port;
    return e;
}

std::string base32_encode(const std::vector<std::uint8_t> &bytes) {
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (std::uint8_t byte : bytes) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            out += kAlphabet[(buffer >> (bits - 5)) & 31u];
            bits -= 5;
        }
    }
    if (bits > 0) out += kAlphabet[(buffer << (5 - bits)) & 31u];
    return out;
}

std::optional<std::vector<std::uint8_t>> base32_decode(const std::string &text) {
    std::vector<std::uint8_t> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '-' || c == ' ') continue;
        const int value = digit_value(c);
        if (value < 0) return std::nullopt;
        buffer = (buffer << 5) | static_cast<std::uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            out.push_back(static_cast<std::uint8_t>(buffer >> (bits - 8)));
            bits -= 8;
        }
    }
    // Leftover bits are padding and must be zero.
    if (bits >= 5 || (buffer & ((1u << bits) - 1u)) != 0u) return std::nullopt;
    return out;
}

std::string encode_invite(const Invite &invite) {
    std::vector<std::uint8_t> bytes;
    bytes.push_back(kInviteVersion);
    bytes.insert(bytes.end(), invite.secret.begin(), invite.secret.end());
    put_endpoints(bytes, invite.candidates);
    const auto check = keyed_hash(invite.secret, bytes.data(), bytes.size(), 16u);
    bytes.push_back(check[0]);
    bytes.push_back(check[1]);
    return grouped(kInvitePrefix, base32_encode(bytes));
}

std::optional<Invite> decode_invite(const std::string &text) {
    const auto body = body_after(text, kInvitePrefix);
    if (!body || looks_like_reply(text) || looks_like_friend(text)) return std::nullopt;
    const auto bytes = base32_decode(*body);
    if (!bytes || bytes->size() < 1u + 16u + 1u + 2u || (*bytes)[0] != kInviteVersion) return std::nullopt;
    Invite invite;
    std::copy(bytes->begin() + 1, bytes->begin() + 17, invite.secret.begin());
    const std::size_t end = bytes->size() - 2u;
    const auto check = keyed_hash(invite.secret, bytes->data(), end, 16u);
    if (check[0] != (*bytes)[end] || check[1] != (*bytes)[end + 1]) return std::nullopt;
    std::size_t at = 17u;
    if (!get_endpoints(*bytes, at, end, invite.candidates) || invite.candidates.empty()) return std::nullopt;
    return invite;
}

std::string encode_reply(const Secret &secret, const std::vector<Endpoint> &joiner) {
    std::vector<std::uint8_t> plain;
    put_endpoints(plain, joiner);
    std::array<std::uint8_t, 8> number_bytes{};
    random_bytes(number_bytes.data(), number_bytes.size());
    std::uint64_t number = 0;
    for (int i = 0; i < 8; ++i) number |= static_cast<std::uint64_t>(number_bytes[i]) << (8 * i);
    static constexpr std::uint8_t kHeader[] = {'V', 'C', '2', 'R'};
    const auto sealed = seal(reply_key(secret), number, kHeader, sizeof(kHeader), plain.data(), plain.size());
    std::vector<std::uint8_t> bytes(number_bytes.begin(), number_bytes.end());
    bytes.insert(bytes.end(), sealed.begin(), sealed.end());
    return grouped(kReplyPrefix, base32_encode(bytes));
}

std::optional<std::vector<Endpoint>> decode_reply(const Secret &secret, const std::string &text) {
    const auto body = body_after(text, kReplyPrefix);
    if (!body) return std::nullopt;
    const auto bytes = base32_decode(*body);
    if (!bytes || bytes->size() < 8u + kSealOverhead + 1u) return std::nullopt;
    std::uint64_t number = 0;
    for (int i = 0; i < 8; ++i) number |= static_cast<std::uint64_t>((*bytes)[i]) << (8 * i);
    static constexpr std::uint8_t kHeader[] = {'V', 'C', '2', 'R'};
    const auto plain = open(reply_key(secret), number, kHeader, sizeof(kHeader), bytes->data() + 8, bytes->size() - 8u);
    if (!plain) return std::nullopt;
    std::vector<Endpoint> out;
    std::size_t at = 0;
    if (!get_endpoints(*plain, at, plain->size(), out) || out.empty()) return std::nullopt;
    return out;
}

bool looks_like_invite(const std::string &text) {
    return body_after(text, kInvitePrefix).has_value() && !looks_like_reply(text) && !looks_like_friend(text);
}
bool looks_like_friend(const std::string &text) { return body_after(text, kFriendPrefix).has_value(); }

std::string encode_friend(const FriendCode &code) {
    std::vector<std::uint8_t> bytes;
    bytes.push_back(kFriendVersion);
    bytes.insert(bytes.end(), code.public_key.begin(), code.public_key.end());
    put_endpoints(bytes, code.candidates);
    const auto check = plain_hash(bytes.data(), bytes.size(), 16u);
    bytes.push_back(check[0]);
    bytes.push_back(check[1]);
    return grouped(kFriendPrefix, base32_encode(bytes));
}

std::optional<FriendCode> decode_friend(const std::string &text) {
    const auto body = body_after(text, kFriendPrefix);
    if (!body) return std::nullopt;
    const auto bytes = base32_decode(*body);
    if (!bytes || bytes->size() < 1u + 32u + 1u + 2u || (*bytes)[0] != kFriendVersion) return std::nullopt;
    const std::size_t end = bytes->size() - 2u;
    const auto check = plain_hash(bytes->data(), end, 16u);
    if (check[0] != (*bytes)[end] || check[1] != (*bytes)[end + 1]) return std::nullopt;
    FriendCode code;
    std::copy(bytes->begin() + 1, bytes->begin() + 33, code.public_key.begin());
    std::size_t at = 33u;
    if (!get_endpoints(*bytes, at, end, code.candidates)) return std::nullopt;
    return code;
}
bool looks_like_reply(const std::string &text) { return body_after(text, kReplyPrefix).has_value(); }

} // namespace portablekit::vc2::online
