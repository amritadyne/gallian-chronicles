#pragma once

// The online session: one UDP socket between the host's PC and each joiner's,
// carrying the ad hoc client's TCP connections to the host's built-in ad hoc
// server, encrypted (online_crypto.hpp).
//
//   joiner's game ─ adhoc::Client ─TCP─▶ 127.0.0.1:A / A+1 (this session's
//        listeners: matchmaking / relay)
//     ═══ UDP, encrypted, reliable streams ═══
//   host:  this session ─TCP─▶ 127.0.0.1:27312 / 27313 (the built-in server,
//        which listens on the loopback address only)
//
// Packets (UDP):
//   HELLO    "VC2H" | version | edition (16) | joiner key (32) | nonce (16)
//            | time ms (8) | tag (32)                              joiner → host
//   WELCOME  "VC2W" | version | status | host key (32) | nonce (16)
//            | connection id (8) | tag (32)                        host → joiner
//   PUNCH    "VC2P" | random (16) | tag (32)       host → joiner (reply codes:
//            opens the host's router to the joiner; the joiner ignores it)
//   DATA     0xD0 | connection id (8) | packet number (8) | sealed frames
// The tags are HMAC under the invite secret's auth key (with friend codes,
// the secret the two players share; the host tries each friend's); HELLO is resent until
// WELCOME comes (the host answers the same nonce with the same WELCOME).
//
// Frames, inside DATA:
//   PING time (8) / PONG time (8)                keep-alive and round trip
//   SEGMENT stream (4) | seq (4) | flags (1) | length (2) | bytes
//            flags: 1 FIN (the sender's side closed), 2 OPEN (seq 0, one
//            byte: 0 matchmaking, 1 relay)
//   ACK      stream (4) | next (4) | mask (8)     next: every seq below it has
//            arrived; mask bit i: seq next + 1 + i has arrived
//   RESET    stream (4)                           drop the stream at once
//   BYE                                           the sender is leaving
//
// A stream is a TCP connection: the joiner opens one for each connection its
// game's ad hoc client makes, and the host makes the same connection to its
// server. Bytes are delivered in order, resent until acknowledged (64
// segments in flight, the resend time from the measured round trip), and a
// close on one side closes the other once everything is delivered.
//
// One network thread does everything; the public calls only exchange state
// with it under a mutex.
#include "online_crypto.hpp"
#include "online_invite.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace portablekit::vc2::online {

constexpr std::uint8_t kProtocolVersion = 1u;
constexpr std::size_t kMaxJoiners = 3u;  // four players in all, VC2's most

enum class Role { Host, Join };

struct SessionConfig {
    Role role{Role::Host};
    Secret secret{};
    // Friend codes: the host accepts any of these secrets (one per friend,
    // online_crypto.hpp pair_secret), each friend's slot in `slots`; empty:
    // `secret` alone. A joiner uses `secret`, its host's slot in `host_slot`.
    std::vector<Secret> secrets;
    std::vector<int> slots;
    int host_slot{-1};
    // Which game and version: sessions only form between the same class.
    std::string edition;
    // The UDP port: the host's (the next free one up to +16 when taken);
    // a joiner's 0 lets the system choose.
    std::uint16_t udp_port{};
    // Host: the built-in server's matchmaking port on 127.0.0.1 (the relay is
    // on the next). Joiner: where its own listeners go (0: the first free pair
    // from 27412).
    std::uint16_t adhocctl_port{};
    // Joiner: the host's addresses, tried all at once.
    std::vector<Endpoint> candidates;
};

struct PeerInfo {
    std::string address;
    int round_trip_ms{-1};
    unsigned streams{};
    int slot{-1};  // the friend's slot (SessionConfig::slots / host_slot), -1 unknown
};

struct SessionStatus {
    bool running{};
    bool connected{};      // joiner: the host answered; host: at least one joiner
    std::string error;     // why start() failed, or why the host refused us
    std::uint16_t udp_port{};
    std::uint16_t local_adhocctl_port{};  // joiner: where its game connects
    std::vector<PeerInfo> peers;
    std::uint64_t bytes_in{}, bytes_out{};
};

// Packets that are not the session's (STUN answers) go to this, on the
// network thread.
using RawHandler = std::function<void(const Endpoint &from, const std::uint8_t *data, std::size_t size)>;

class Session {
public:
    Session();
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    bool start(const SessionConfig &config);
    void stop();
    [[nodiscard]] SessionStatus status() const;

    // Sends a datagram from the session's socket (STUN requests, so the
    // address STUN reports is this socket's).
    void send_raw(const Endpoint &to, std::vector<std::uint8_t> data);
    void set_raw_handler(RawHandler handler);

    // Host: send PUNCH packets to these for 20 s (a joiner's reply code), so
    // the host's router lets their HELLOs in.
    void punch(const std::vector<Endpoint> &targets);
    // Joiner: more host addresses to try.
    void add_candidates(const std::vector<Endpoint> &more);

    // Log lines go here as well as to the console, if set.
    static void set_log(std::function<void(const std::string &)> sink);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace portablekit::vc2::online
