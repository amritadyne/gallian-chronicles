#include "online_session.hpp"

#include "adhoc/sockets.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#if defined(_WIN32)
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

namespace portablekit::vc2::online {
namespace {

using namespace adhoc::net;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// Sizes. A datagram stays under 1280 bytes, the smallest IPv6 path, with room
// for tunnels (VPNs) underneath.
constexpr std::size_t kMaxPlain = 1150u;     // frames per packet
constexpr std::size_t kSegmentMax = 1100u;   // stream bytes per segment
constexpr std::size_t kDataHeader = 17u;     // 0xD0 | id | number
constexpr std::size_t kHelloSize = 109u;
constexpr std::size_t kWelcomeSize = 94u;
constexpr std::size_t kPunchSize = 52u;
constexpr std::size_t kWindow = 64u;               // segments in flight per stream
constexpr std::size_t kReadAhead = 64u * 1024u;    // TCP bytes read before they are sent
constexpr std::size_t kDeliverLimit = 1024u * 1024u;  // bytes held for a slow TCP side
constexpr std::size_t kMaxStreams = 64u;           // per peer
constexpr std::uint32_t kReorderLimit = 256u;      // segments ahead of the next expected

constexpr auto kTick = milliseconds(4);
constexpr auto kHelloEvery = milliseconds(250);
constexpr auto kPingEvery = milliseconds(1000);
constexpr auto kPeerTimeout = milliseconds(15000);
constexpr auto kPunchFor = milliseconds(20000);
constexpr auto kPunchEvery = milliseconds(200);
constexpr auto kTcpConnectTimeout = milliseconds(5000);
constexpr auto kClosingLimit = milliseconds(30000);

enum Frame : std::uint8_t { kPing = 1, kPong = 2, kSegmentFrame = 3, kAck = 4, kReset = 5, kBye = 6 };
enum Flags : std::uint8_t { kFin = 1, kOpen = 2 };
enum Status : std::uint8_t { kOk = 0, kFull = 1, kWrongEdition = 2, kWrongVersion = 3 };

std::mutex g_log_mutex;
std::function<void(const std::string &)> g_log_sink;

void log(const std::string &line) {
    std::cerr << "[vc2-online] " << line << "\n";
    std::lock_guard lock(g_log_mutex);
    if (g_log_sink) g_log_sink(line);
}

// Big-endian integers.
void put(std::vector<std::uint8_t> &out, std::uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
std::uint64_t get(const std::uint8_t *in, int bytes) {
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) value = (value << 8) | in[i];
    return value;
}

std::uint64_t now_ms_wall() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint64_t now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

std::array<std::uint8_t, 16> edition_field(const std::string &edition) {
    std::array<std::uint8_t, 16> field{};
    std::memcpy(field.data(), edition.data(), std::min(edition.size(), field.size()));
    return field;
}

Endpoint from_address(const Address &address) {
    Endpoint e;
    e.port = address.port();
    if (const std::uint32_t v4 = address.ipv4(); v4 != 0u || address.family() == AF_INET) {
        std::memcpy(e.ip.data(), &v4, 4);
        return e;
    }
    e.v6 = true;
    std::memcpy(e.ip.data(), reinterpret_cast<const sockaddr_in6 *>(&address.storage)->sin6_addr.s6_addr, 16);
    return e;
}

// For a socket of `family`; nullopt when it cannot reach the endpoint.
std::optional<Address> to_address(const Endpoint &e, int family) {
    Address address;
    if (family == AF_INET) {
        if (e.v6) return std::nullopt;
        std::uint32_t v4 = 0;
        std::memcpy(&v4, e.ip.data(), 4);
        return Address::ipv4_address(v4, e.port);
    }
    auto *in6 = reinterpret_cast<sockaddr_in6 *>(&address.storage);
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(e.port);
    if (e.v6) {
        std::memcpy(in6->sin6_addr.s6_addr, e.ip.data(), 16);
    } else {
        std::memset(in6->sin6_addr.s6_addr, 0, 10);
        in6->sin6_addr.s6_addr[10] = 0xFF;
        in6->sin6_addr.s6_addr[11] = 0xFF;
        std::memcpy(in6->sin6_addr.s6_addr + 12, e.ip.data(), 4);
    }
    address.length = sizeof(sockaddr_in6);
    return address;
}

void close_hard(Socket s) {
    if (s == kNoSocket) return;
    linger option{};
    option.l_onoff = 1;
    option.l_linger = 0;
    setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&option), sizeof(option));
    close_socket(s);
}

void shutdown_send(Socket s) {
#if defined(_WIN32)
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
}

Socket listen_loopback(std::uint16_t port) {
    Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) return kNoSocket;
#if defined(_WIN32)
    set_option(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
    set_option(s, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    const Address address = Address::ipv4_address(htonl(INADDR_LOOPBACK), port);
    if (::bind(s, reinterpret_cast<const sockaddr *>(&address.storage), address.length) != 0 ||
        ::listen(s, 16) != 0 || !set_nonblocking(s)) {
        close_socket(s);
        return kNoSocket;
    }
    return s;
}

struct Segment {
    std::uint8_t flags{};
    std::string bytes;
    Clock::time_point sent{};
    int sends{};
};

struct Stream {
    std::uint8_t target{};
    Socket tcp{kNoSocket};
    bool connecting{};          // host: the TCP connect is under way
    Clock::time_point connect_started{};
    bool opened{};              // the OPEN segment arrived (host) or was sent (joiner)
    bool tcp_eof{};             // the TCP side will send no more
    bool tcp_shut{};            // FIN passed on to the TCP side
    // Sending.
    std::string pending;        // read from TCP, not yet in a segment
    std::uint32_t next_seq{};
    std::map<std::uint32_t, Segment> in_flight;
    std::uint32_t highest_sacked{};
    bool fin_sent{};
    // Receiving.
    std::string to_tcp;         // in order, not yet written to TCP
    std::uint32_t next_expected{};
    std::map<std::uint32_t, Segment> early;
    bool fin_received{};
    bool ack_due{};
    Clock::time_point closing_since{};
};

struct Peer {
    std::uint64_t id{};
    Endpoint endpoint;
    Address address;
    TrafficKeys keys;
    std::uint64_t send_number{1};
    ReplayWindow replay;
    Clock::time_point last_heard{};
    Clock::time_point last_ping{};
    double srtt_ms{200.0};
    int rtt_ms{-1};
    std::map<std::uint32_t, Stream> streams;
    std::set<std::uint32_t> closed;
    std::uint32_t highest_stream{};
    std::uint32_t next_stream{1};
    std::vector<std::uint64_t> pongs;     // ping times to echo
    std::vector<std::uint32_t> resets;    // RESET frames to send
    std::vector<std::pair<std::uint32_t, std::uint32_t>> final_acks;  // closed streams' last ACKs
    int secret_index{};  // host: which of the accepted secrets
    int slot{-1};
    // Host: the HELLO this peer came from, and the WELCOME sent back.
    std::array<std::uint8_t, 16> hello_nonce{};
    std::vector<std::uint8_t> welcome;

    [[nodiscard]] milliseconds resend_time() const {
        const auto value = static_cast<long long>(srtt_ms * 2.0 + 30.0);
        return milliseconds(std::clamp<long long>(value, 50, 1000));
    }
};

} // namespace

struct Session::Impl {
    WinsockSession winsock;
    mutable std::mutex mutex;
    std::thread thread;
    std::atomic<bool> quit{false};
    SessionConfig config;
    Key auth{};
    std::vector<Secret> secrets;  // host: every accepted secret; joiner: its one
    std::vector<Key> auths;
    Socket udp{kNoSocket};
    int family{AF_INET6};
    Socket listeners[2]{kNoSocket, kNoSocket};
    std::map<std::uint64_t, Peer> peers;   // host: by connection id; joiner: the host
    RawHandler raw_handler;
    std::vector<std::pair<Endpoint, std::vector<std::uint8_t>>> raw_out;
    std::vector<Endpoint> punch_targets;
    Clock::time_point punch_until{};
    Clock::time_point last_punch{};
    std::vector<Endpoint> extra_candidates;
    SessionStatus snapshot;
    Clock::time_point snapshot_at{};
    std::uint64_t bytes_in{}, bytes_out{};
    // Joiner.
    std::optional<KeyPair> hello_keys;
    std::vector<std::uint8_t> hello;
    Clock::time_point last_hello{};
    bool refused{};
    std::string refusal;
    Clock::time_point retry_at{};
    // Host: HELLOs checked this second, for a cheap limit on forged floods.
    Clock::time_point hello_second{};
    unsigned hellos_this_second{};

    // Sockets ---------------------------------------------------------------

    bool open_udp(std::uint16_t first_port, bool search) {
        for (int attempt = 0; attempt <= (search ? 16 : 0); ++attempt) {
            const auto port = static_cast<std::uint16_t>(first_port == 0u ? 0u : first_port + attempt);
            Socket s = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
            int fam = AF_INET6;
            if (s != kNoSocket) {
                set_option(s, IPPROTO_IPV6, IPV6_V6ONLY, 0);
                sockaddr_in6 address{};
                address.sin6_family = AF_INET6;
                address.sin6_addr = in6addr_any;
                address.sin6_port = htons(port);
                if (::bind(s, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
                    close_socket(s);
                    s = kNoSocket;
                }
            }
            if (s == kNoSocket) {
                fam = AF_INET;
                s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (s == kNoSocket) continue;
                const Address address = Address::ipv4_address(htonl(INADDR_ANY), port);
                if (::bind(s, reinterpret_cast<const sockaddr *>(&address.storage), address.length) != 0) {
                    close_socket(s);
                    continue;
                }
            }
            if (!set_nonblocking(s)) {
                close_socket(s);
                continue;
            }
#if defined(_WIN32)
            // An ICMP "port unreachable" would otherwise fail the next receive.
            BOOL report = FALSE;
            DWORD returned = 0;
            WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#endif
            set_option(s, SOL_SOCKET, SO_RCVBUF, 1 << 20);
            set_option(s, SOL_SOCKET, SO_SNDBUF, 1 << 20);
            udp = s;
            family = fam;
            return true;
        }
        return false;
    }

    std::uint16_t udp_port() const {
        Address address;
        address.length = sizeof(address.storage);
        if (getsockname(udp, reinterpret_cast<sockaddr *>(&address.storage), &address.length) != 0) return 0u;
        return address.port();
    }

    void send_to(const Address &address, const std::uint8_t *data, std::size_t size) {
        if (::sendto(udp, reinterpret_cast<const char *>(data), static_cast<int>(size), 0,
                     reinterpret_cast<const sockaddr *>(&address.storage), address.length) > 0)
            bytes_out += size;
    }
    void send_to(const Endpoint &endpoint, const std::vector<std::uint8_t> &data) {
        if (const auto address = to_address(endpoint, family)) send_to(*address, data.data(), data.size());
    }

    // Packets ----------------------------------------------------------------

    void send_frames(Peer &peer, const std::vector<std::uint8_t> &frames) {
        std::vector<std::uint8_t> header;
        header.push_back(0xD0u);
        put(header, peer.id, 8);
        put(header, peer.send_number, 8);
        const auto sealed = seal(peer.keys.send, peer.send_number, header.data(), header.size(), frames.data(),
                                 frames.size());
        ++peer.send_number;
        header.insert(header.end(), sealed.begin(), sealed.end());
        send_to(peer.address, header.data(), header.size());
    }

    // Collects frames into packets of at most kMaxPlain bytes.
    struct Packer {
        Impl &impl;
        Peer &peer;
        std::vector<std::uint8_t> frames;
        void add(const std::vector<std::uint8_t> &frame) {
            if (!frames.empty() && frames.size() + frame.size() > kMaxPlain) flush();
            frames.insert(frames.end(), frame.begin(), frame.end());
        }
        void flush() {
            if (frames.empty()) return;
            impl.send_frames(peer, frames);
            frames.clear();
        }
    };

    static std::vector<std::uint8_t> segment_frame(std::uint32_t stream, std::uint32_t seq, const Segment &segment) {
        std::vector<std::uint8_t> frame;
        frame.push_back(kSegmentFrame);
        put(frame, stream, 4);
        put(frame, seq, 4);
        frame.push_back(segment.flags);
        put(frame, segment.bytes.size(), 2);
        frame.insert(frame.end(), segment.bytes.begin(), segment.bytes.end());
        return frame;
    }

    // Joiner: HELLO ------------------------------------------------------------

    void new_hello() {
        hello_keys.emplace();
        hello.clear();
        hello.insert(hello.end(), {'V', 'C', '2', 'H'});
        hello.push_back(kProtocolVersion);
        const auto edition = edition_field(config.edition);
        hello.insert(hello.end(), edition.begin(), edition.end());
        hello.insert(hello.end(), hello_keys->public_key.begin(), hello_keys->public_key.end());
        std::array<std::uint8_t, 16> nonce{};
        random_bytes(nonce.data(), nonce.size());
        hello.insert(hello.end(), nonce.begin(), nonce.end());
        put(hello, now_ms_wall(), 8);
        std::uint8_t mac[32];
        tag(auth, hello.data(), hello.size(), mac);
        hello.insert(hello.end(), mac, mac + 32);
    }

    std::vector<Endpoint> all_candidates() {
        std::vector<Endpoint> all = config.candidates;
        std::lock_guard lock(mutex);
        for (const Endpoint &e : extra_candidates)
            if (std::find(all.begin(), all.end(), e) == all.end()) all.push_back(e);
        return all;
    }

    void joiner_tick(Clock::time_point now) {
        if (!peers.empty() || hello.empty()) return;
        if (refused && now < retry_at) return;
        if (now - last_hello < kHelloEvery) return;
        last_hello = now;
        for (const Endpoint &e : all_candidates()) send_to(e, hello);
    }

    void on_welcome(const std::uint8_t *data, std::size_t size, const Address &from) {
        if (config.role != Role::Join || !peers.empty() || hello.empty() || size != kWelcomeSize) return;
        if (!tag_ok(auth, data, 62u, data + 62)) return;
        if (std::memcmp(data + 38, hello.data() + 53, 16) != 0) return;  // not an answer to our HELLO
        const std::uint8_t status = data[5];
        if (status != kOk) {
            if (!refused) {
                refusal = status == kFull           ? "the session is full"
                          : status == kWrongEdition ? "the host plays a different version of the game"
                                                    : "the host runs a different version of online play";
                log("the host refused: " + refusal);
            }
            refused = true;
            retry_at = Clock::now() + milliseconds(5000);
            return;
        }
        Key host_public{};
        std::memcpy(host_public.data(), data + 6, 32);
        std::vector<std::uint8_t> transcript = hello;
        transcript.insert(transcript.end(), data, data + 62);
        auto keys = joiner_keys(config.secret, *hello_keys, host_public, transcript);
        if (!keys) return;
        Peer &peer = peers[get(data + 54, 8)];
        peer.id = get(data + 54, 8);
        peer.address = from;
        peer.endpoint = from_address(from);
        peer.keys.send = keys->send;
        peer.keys.receive = keys->receive;
        peer.last_heard = Clock::now();
        peer.slot = config.host_slot;
        refused = false;
        refusal.clear();
        log("connected to the host at " + peer.endpoint.describe());
    }

    // Host: HELLO --------------------------------------------------------------

    void on_hello(const std::uint8_t *data, std::size_t size, const Address &from) {
        if (config.role != Role::Host || size != kHelloSize) return;
        const auto now = Clock::now();
        if (now - hello_second >= milliseconds(1000)) {
            hello_second = now;
            hellos_this_second = 0;
        }
        if (++hellos_this_second > 50u) return;
        int which = -1;
        for (std::size_t i = 0; i < auths.size() && which < 0; ++i)
            if (tag_ok(auths[i], data, 77u, data + 77)) which = static_cast<int>(i);
        if (which < 0) return;
        const Key &friend_auth = auths[static_cast<std::size_t>(which)];
        const Secret &secret = secrets[static_cast<std::size_t>(which)];
        std::array<std::uint8_t, 16> nonce{};
        std::memcpy(nonce.data(), data + 53, 16);
        // A resent HELLO: the same WELCOME again, to wherever it now comes from.
        for (auto &[id, peer] : peers) {
            if (peer.hello_nonce == nonce) {
                send_to(from, peer.welcome.data(), peer.welcome.size());
                return;
            }
        }
        const std::uint64_t sent = get(data + 69, 8);
        const std::uint64_t wall = now_ms_wall();
        const std::uint64_t skew = sent > wall ? sent - wall : wall - sent;
        if (skew > 24ull * 3600ull * 1000ull) return;  // a very old recording
        std::uint8_t status = kOk;
        if (data[4] != kProtocolVersion) status = kWrongVersion;
        else if (std::memcmp(data + 5, edition_field(config.edition).data(), 16) != 0) status = kWrongEdition;
        else if (std::count_if(peers.begin(), peers.end(), [&](const auto &p) {
                     return config.secrets.empty() || p.second.secret_index != which;
                 }) >= static_cast<std::ptrdiff_t>(kMaxJoiners))
            status = kFull;
        const KeyPair own;
        std::uint64_t id = 0;
        random_bytes(&id, sizeof(id));
        std::vector<std::uint8_t> welcome{'V', 'C', '2', 'W', kProtocolVersion, status};
        welcome.insert(welcome.end(), own.public_key.begin(), own.public_key.end());
        welcome.insert(welcome.end(), nonce.begin(), nonce.end());
        put(welcome, id, 8);
        std::uint8_t mac[32];
        tag(friend_auth, welcome.data(), welcome.size(), mac);
        std::vector<std::uint8_t> transcript(data, data + kHelloSize);
        transcript.insert(transcript.end(), welcome.begin(), welcome.end());
        welcome.insert(welcome.end(), mac, mac + 32);
        if (status != kOk) {
            send_to(from, welcome.data(), welcome.size());
            log("refused a joiner at " + from_address(from).describe() + " (status " + std::to_string(status) + ")");
            return;
        }
        Key joiner_public{};
        std::memcpy(joiner_public.data(), data + 21, 32);
        auto keys = host_keys(secret, own, joiner_public, transcript);
        if (!keys) return;
        // A friend's new handshake (their game restarted): the old connection goes.
        if (config.secrets.size() > 0u) {
            std::vector<std::uint64_t> old;
            for (const auto &[pid, p] : peers)
                if (p.secret_index == which) old.push_back(pid);
            for (const std::uint64_t pid : old) drop_peer(pid, "reconnected");
        }
        Peer &peer = peers[id];
        peer.secret_index = which;
        peer.slot = which < static_cast<int>(config.slots.size()) ? config.slots[static_cast<std::size_t>(which)] : -1;
        peer.id = id;
        peer.address = from;
        peer.endpoint = from_address(from);
        peer.keys.send = keys->send;
        peer.keys.receive = keys->receive;
        peer.last_heard = now;
        peer.hello_nonce = nonce;
        peer.welcome = welcome;
        send_to(from, welcome.data(), welcome.size());
        log("a player joined from " + peer.endpoint.describe() + " (" + std::to_string(peers.size()) + " of " +
            std::to_string(kMaxJoiners) + ")");
    }

    // DATA ---------------------------------------------------------------------

    void on_data(const std::uint8_t *data, std::size_t size, const Address &from) {
        if (size < kDataHeader + kSealOverhead) return;
        const std::uint64_t id = get(data + 1, 8);
        auto found = peers.find(id);
        if (found == peers.end()) return;
        Peer &peer = found->second;
        const std::uint64_t number = get(data + 9, 8);
        const auto plain = open(peer.keys.receive, number, data, kDataHeader, data + kDataHeader, size - kDataHeader);
        if (!plain || !peer.replay.accept(number)) return;
        peer.last_heard = Clock::now();
        if (from_address(from) != peer.endpoint) {
            log("the other side moved to " + from_address(from).describe());
            peer.address = from;
            peer.endpoint = from_address(from);
        }
        read_frames(peer, *plain);
    }

    void read_frames(Peer &peer, const std::vector<std::uint8_t> &plain) {
        std::size_t at = 0;
        const std::size_t end = plain.size();
        const std::uint8_t *p = plain.data();
        while (at < end) {
            const std::uint8_t type = p[at++];
            switch (type) {
            case kPing:
                if (end - at < 8u) return;
                peer.pongs.push_back(get(p + at, 8));
                at += 8;
                break;
            case kPong: {
                if (end - at < 8u) return;
                const std::uint64_t sent = get(p + at, 8);
                at += 8;
                const std::uint64_t now = now_us();
                if (sent <= now && now - sent < 10'000'000u) {
                    const double rtt = static_cast<double>(now - sent) / 1000.0;
                    peer.srtt_ms = peer.rtt_ms < 0 ? rtt : peer.srtt_ms * 0.875 + rtt * 0.125;
                    peer.rtt_ms = static_cast<int>(peer.srtt_ms + 0.5);
                }
                break;
            }
            case kSegmentFrame: {
                if (end - at < 11u) return;
                const auto stream = static_cast<std::uint32_t>(get(p + at, 4));
                const auto seq = static_cast<std::uint32_t>(get(p + at + 4, 4));
                Segment segment;
                segment.flags = p[at + 8];
                const auto length = static_cast<std::size_t>(get(p + at + 9, 2));
                at += 11;
                if (end - at < length) return;
                segment.bytes.assign(reinterpret_cast<const char *>(p + at), length);
                at += length;
                on_segment(peer, stream, seq, std::move(segment));
                break;
            }
            case kAck: {
                if (end - at < 16u) return;
                const auto stream = static_cast<std::uint32_t>(get(p + at, 4));
                const auto next = static_cast<std::uint32_t>(get(p + at + 4, 4));
                const std::uint64_t mask = get(p + at + 8, 8);
                at += 16;
                on_ack(peer, stream, next, mask);
                break;
            }
            case kReset: {
                if (end - at < 4u) return;
                const auto stream = static_cast<std::uint32_t>(get(p + at, 4));
                at += 4;
                if (auto found = peer.streams.find(stream); found != peer.streams.end()) {
                    close_hard(found->second.tcp);
                    peer.closed.insert(stream);
                    peer.streams.erase(found);
                }
                break;
            }
            case kBye:
                peer.last_heard = Clock::time_point{};  // dropped on the next check
                return;
            default:
                return;  // an unknown frame: the rest cannot be read
            }
        }
    }

    void on_segment(Peer &peer, std::uint32_t id, std::uint32_t seq, Segment segment) {
        auto found = peer.streams.find(id);
        if (found == peer.streams.end()) {
            // Only the joiner opens streams; a stream we closed stays closed.
            const bool can_open = config.role == Role::Host && !peer.closed.count(id) && id > 0u &&
                                  peer.streams.size() < kMaxStreams;
            if (!can_open) {
                if (peer.closed.count(id) || config.role == Role::Join || id == 0u)
                    peer.resets.push_back(id);
                return;
            }
            found = peer.streams.emplace(id, Stream{}).first;
            peer.highest_stream = std::max(peer.highest_stream, id);
        }
        Stream &stream = found->second;
        stream.ack_due = true;
        if (seq < stream.next_expected) return;  // a resend we already have
        if (seq - stream.next_expected >= kReorderLimit) return;
        if (stream.to_tcp.size() > kDeliverLimit) return;  // resent later
        if (seq != stream.next_expected) {
            stream.early.emplace(seq, std::move(segment));
            return;
        }
        deliver(peer, id, stream, std::move(segment));
        while (true) {
            auto next = stream.early.find(stream.next_expected);
            if (next == stream.early.end()) break;
            Segment ready = std::move(next->second);
            stream.early.erase(next);
            deliver(peer, id, stream, std::move(ready));
        }
    }

    void deliver(Peer &peer, std::uint32_t id, Stream &stream, Segment segment) {
        ++stream.next_expected;
        if ((segment.flags & kOpen) != 0u) {
            if (config.role == Role::Host && !stream.opened && segment.bytes.size() == 1u &&
                static_cast<std::uint8_t>(segment.bytes[0]) <= 1u) {
                stream.opened = true;
                stream.target = static_cast<std::uint8_t>(segment.bytes[0]);
                connect_server(peer, id, stream);
            }
            return;
        }
        stream.to_tcp += segment.bytes;
        if ((segment.flags & kFin) != 0u) stream.fin_received = true;
    }

    void on_ack(Peer &peer, std::uint32_t id, std::uint32_t next, std::uint64_t mask) {
        auto found = peer.streams.find(id);
        if (found == peer.streams.end()) return;
        Stream &stream = found->second;
        stream.in_flight.erase(stream.in_flight.begin(), stream.in_flight.lower_bound(next));
        for (int bit = 0; bit < 64; ++bit) {
            if (((mask >> bit) & 1u) == 0u) continue;
            const std::uint32_t seq = next + 1u + static_cast<std::uint32_t>(bit);
            stream.in_flight.erase(seq);
            stream.highest_sacked = std::max(stream.highest_sacked, seq);
        }
    }

    // Host: its own server -------------------------------------------------------

    void connect_server(Peer &peer, std::uint32_t id, Stream &stream) {
        const auto port = static_cast<std::uint16_t>(config.adhocctl_port + stream.target);
        const Address address = Address::ipv4_address(htonl(INADDR_LOOPBACK), port);
        Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kNoSocket || !set_nonblocking(s)) {
            if (s != kNoSocket) close_socket(s);
            peer.resets.push_back(id);
            return;
        }
        set_option(s, IPPROTO_TCP, TCP_NODELAY, 1);
        no_sigpipe(s);
        stream.tcp = s;
        stream.connect_started = Clock::now();
        if (::connect(s, reinterpret_cast<const sockaddr *>(&address.storage), address.length) == 0) return;
        if (connect_pending(socket_error())) {
            stream.connecting = true;
            return;
        }
        close_socket(s);
        stream.tcp = kNoSocket;
        peer.resets.push_back(id);
    }

    // Joiner: its listeners --------------------------------------------------------

    bool open_listeners() {
        const std::uint16_t first = config.adhocctl_port != 0u ? config.adhocctl_port : 27412u;
        const int tries = config.adhocctl_port != 0u ? 1 : 40;
        for (int i = 0; i < tries; ++i) {
            const auto port = static_cast<std::uint16_t>(first + 2 * i);
            Socket a = listen_loopback(port);
            if (a == kNoSocket) continue;
            Socket b = listen_loopback(static_cast<std::uint16_t>(port + 1u));
            if (b == kNoSocket) {
                close_socket(a);
                continue;
            }
            listeners[0] = a;
            listeners[1] = b;
            std::lock_guard lock(mutex);
            snapshot.local_adhocctl_port = port;
            return true;
        }
        return false;
    }

    void accept_local(int which) {
        for (int round = 0; round < 8; ++round) {
            const Socket s = ::accept(listeners[which], nullptr, nullptr);
            if (s == kNoSocket) return;
            if (peers.empty()) {  // no host yet: the game's client retries
                close_hard(s);
                continue;
            }
            Peer &peer = peers.begin()->second;
            if (peer.streams.size() >= kMaxStreams || !set_nonblocking(s)) {
                close_hard(s);
                continue;
            }
            set_option(s, IPPROTO_TCP, TCP_NODELAY, 1);
            no_sigpipe(s);
            const std::uint32_t id = peer.next_stream++;
            Stream &stream = peer.streams[id];
            stream.tcp = s;
            stream.target = static_cast<std::uint8_t>(which);
            stream.opened = true;
            Segment open_segment;
            open_segment.flags = kOpen;
            open_segment.bytes.assign(1u, static_cast<char>(which));
            stream.in_flight.emplace(stream.next_seq++, std::move(open_segment));
        }
    }

    // TCP side of the streams -------------------------------------------------------

    // Reads and writes what it can; false when the stream is finished with.
    bool service_tcp(Peer &peer, std::uint32_t id, Stream &stream, short revents) {
        const auto now = Clock::now();
        if (stream.tcp == kNoSocket) return true;  // host: waiting for the OPEN segment
        if (stream.connecting) {
            if ((revents & (POLLERR | POLLHUP)) != 0 || now - stream.connect_started > kTcpConnectTimeout) {
                peer.resets.push_back(id);
                return false;
            }
            if ((revents & POLLOUT) == 0) return true;
            int error = 0;
            socklen_t length = sizeof(error);
            getsockopt(stream.tcp, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length);
            if (error != 0) {
                peer.resets.push_back(id);
                return false;
            }
            stream.connecting = false;
        }
        // Write what arrived.
        while (!stream.to_tcp.empty()) {
            const int sent = ::send(stream.tcp, stream.to_tcp.data(),
                                    static_cast<int>(std::min<std::size_t>(stream.to_tcp.size(), 64u * 1024u)),
                                    kSendFlags);
            if (sent > 0) {
                stream.to_tcp.erase(0, static_cast<std::size_t>(sent));
                continue;
            }
            if (sent < 0 && would_block(socket_error())) break;
            peer.resets.push_back(id);
            return false;
        }
        if (stream.fin_received && stream.to_tcp.empty() && !stream.tcp_shut) {
            shutdown_send(stream.tcp);
            stream.tcp_shut = true;
        }
        // Read what the TCP side sends.
        if (!stream.tcp_eof && (revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            char buffer[16384];
            while (stream.pending.size() < kReadAhead) {
                const int got = ::recv(stream.tcp, buffer, sizeof(buffer), 0);
                if (got > 0) {
                    stream.pending.append(buffer, static_cast<std::size_t>(got));
                    continue;
                }
                if (got == 0) {
                    stream.tcp_eof = true;
                    break;
                }
                if (would_block(socket_error())) break;
                peer.resets.push_back(id);
                return false;
            }
        }
        // Finished: both sides closed and everything delivered and acknowledged.
        const bool done = stream.tcp_eof && stream.fin_sent && stream.in_flight.empty() && stream.pending.empty() &&
                          stream.fin_received && stream.to_tcp.empty();
        if (done) return false;
        if (stream.tcp_eof || stream.fin_received) {
            if (stream.closing_since == Clock::time_point{}) stream.closing_since = now;
            if (now - stream.closing_since > kClosingLimit) {
                peer.resets.push_back(id);
                return false;
            }
        }
        return true;
    }

    // Sending to a peer -------------------------------------------------------------

    void send_tick(Peer &peer, Clock::time_point now) {
        Packer packer{*this, peer, {}};
        for (std::uint64_t time : peer.pongs) {
            std::vector<std::uint8_t> frame{kPong};
            put(frame, time, 8);
            packer.add(frame);
        }
        peer.pongs.clear();
        if (now - peer.last_ping >= kPingEvery) {
            peer.last_ping = now;
            std::vector<std::uint8_t> frame{kPing};
            put(frame, now_us(), 8);
            packer.add(frame);
        }
        for (const auto &[id, next] : peer.final_acks) {
            std::vector<std::uint8_t> frame{kAck};
            put(frame, id, 4);
            put(frame, next, 4);
            put(frame, 0u, 8);
            packer.add(frame);
        }
        peer.final_acks.clear();
        for (std::uint32_t id : peer.resets) {
            std::vector<std::uint8_t> frame{kReset};
            put(frame, id, 4);
            packer.add(frame);
        }
        peer.resets.clear();
        const auto resend = peer.resend_time();
        const auto quick = milliseconds(static_cast<long long>(peer.srtt_ms * 1.25) + 10);
        for (auto &[id, stream] : peer.streams) {
            if (stream.ack_due) {
                stream.ack_due = false;
                std::uint64_t mask = 0;
                for (const auto &[seq, segment] : stream.early) {
                    const std::uint32_t offset = seq - stream.next_expected - 1u;
                    if (seq > stream.next_expected && offset < 64u) mask |= std::uint64_t{1} << offset;
                }
                std::vector<std::uint8_t> frame{kAck};
                put(frame, id, 4);
                put(frame, stream.next_expected, 4);
                put(frame, mask, 8);
                packer.add(frame);
            }
            // New segments, while the window has room.
            while (stream.in_flight.size() < kWindow && !stream.pending.empty()) {
                Segment segment;
                const std::size_t take = std::min(stream.pending.size(), kSegmentMax);
                segment.bytes = stream.pending.substr(0, take);
                stream.pending.erase(0, take);
                stream.in_flight.emplace(stream.next_seq++, std::move(segment));
            }
            if (stream.tcp_eof && stream.pending.empty() && !stream.fin_sent && stream.in_flight.size() < kWindow) {
                Segment fin;
                fin.flags = kFin;
                stream.in_flight.emplace(stream.next_seq++, std::move(fin));
                stream.fin_sent = true;
            }
            for (auto &[seq, segment] : stream.in_flight) {
                // Resends back off (doubling, to eight times), so a dead path
                // does not flood; one that later segments overtook goes again
                // sooner.
                bool due = segment.sends == 0;
                if (!due) {
                    const auto wait = resend * std::min(1 << std::min(segment.sends - 1, 3), 8);
                    due = now - segment.sent >= wait ||
                          (segment.sends == 1 && seq < stream.highest_sacked && now - segment.sent >= quick);
                }
                if (!due) continue;
                segment.sent = now;
                ++segment.sends;
                packer.add(segment_frame(id, seq, segment));
            }
        }
        packer.flush();
    }

    void drop_peer(std::uint64_t id, const std::string &why) {
        auto found = peers.find(id);
        if (found == peers.end()) return;
        for (auto &[sid, stream] : found->second.streams) close_hard(stream.tcp);
        log((config.role == Role::Host ? "a player left (" : "lost the host (") + why + ")");
        peers.erase(found);
        if (config.role == Role::Join) {
            new_hello();  // reconnect with a new handshake
            last_hello = Clock::time_point{};
        }
    }

    void say_bye() {
        for (auto &[id, peer] : peers) {
            std::vector<std::uint8_t> frame{kBye};
            send_frames(peer, frame);
            for (auto &[sid, stream] : peer.streams) close_hard(stream.tcp);
        }
        peers.clear();
    }

    // The thread ----------------------------------------------------------------------

    void receive_all() {
        std::uint8_t buffer[2048];
        for (int round = 0; round < 256; ++round) {
            Address from;
            from.length = sizeof(from.storage);
            const int got = static_cast<int>(::recvfrom(udp, reinterpret_cast<char *>(buffer), sizeof(buffer), 0,
                                                        reinterpret_cast<sockaddr *>(&from.storage), &from.length));
            if (got < 0) {
                if (would_block(socket_error())) return;
                continue;  // a stray error on one datagram (Windows)
            }
            bytes_in += static_cast<std::uint64_t>(got);
            const auto size = static_cast<std::size_t>(got);
            if (size >= 1u && buffer[0] == 0xD0u) on_data(buffer, size, from);
            else if (size >= 4u && std::memcmp(buffer, "VC2H", 4) == 0) on_hello(buffer, size, from);
            else if (size >= 4u && std::memcmp(buffer, "VC2W", 4) == 0) on_welcome(buffer, size, from);
            else if (size >= 4u && std::memcmp(buffer, "VC2P", 4) == 0) continue;
            else {
                RawHandler handler;
                {
                    std::lock_guard lock(mutex);
                    handler = raw_handler;
                }
                if (handler) handler(from_address(from), buffer, size);
            }
        }
    }

    void punch_tick(Clock::time_point now) {
        std::vector<Endpoint> targets;
        {
            std::lock_guard lock(mutex);
            if (now >= punch_until || now - last_punch < kPunchEvery) return;
            last_punch = now;
            targets = punch_targets;
        }
        std::vector<std::uint8_t> packet{'V', 'C', '2', 'P'};
        std::array<std::uint8_t, 16> noise{};
        random_bytes(noise.data(), noise.size());
        packet.insert(packet.end(), noise.begin(), noise.end());
        std::uint8_t mac[32];
        tag(auth, packet.data(), packet.size(), mac);
        packet.insert(packet.end(), mac, mac + 32);
        for (const Endpoint &e : targets) send_to(e, packet);
    }

    void publish(Clock::time_point now) {
        if (now - snapshot_at < milliseconds(100)) return;
        snapshot_at = now;
        std::lock_guard lock(mutex);
        snapshot.connected = !peers.empty();
        snapshot.error = refusal;
        snapshot.peers.clear();
        for (const auto &[id, peer] : peers) {
            PeerInfo info;
            info.address = peer.endpoint.describe();
            info.round_trip_ms = peer.rtt_ms;
            info.streams = static_cast<unsigned>(peer.streams.size());
            info.slot = peer.slot;
            snapshot.peers.push_back(info);
        }
        snapshot.bytes_in = bytes_in;
        snapshot.bytes_out = bytes_out;
    }

    void run() {
        std::vector<PollEntry> entries;
        struct Owner {
            std::uint64_t peer;
            std::uint32_t stream;
        };
        std::vector<Owner> owners;
        while (!quit.load()) {
            const auto now = Clock::now();
            // Raw sends from other threads.
            std::vector<std::pair<Endpoint, std::vector<std::uint8_t>>> raw;
            {
                std::lock_guard lock(mutex);
                raw.swap(raw_out);
            }
            for (const auto &[to, data] : raw) send_to(to, data);
            if (config.role == Role::Join) joiner_tick(now);
            punch_tick(now);
            // Timeouts.
            for (auto it = peers.begin(); it != peers.end();) {
                const auto id = it->first;
                ++it;
                if (now - peers[id].last_heard > kPeerTimeout) drop_peer(id, "no answer for 15 s");
            }
            for (auto &[id, peer] : peers) send_tick(peer, now);
            publish(now);
            // Wait for the sockets.
            entries.clear();
            owners.clear();
            PollEntry entry{};
            entry.fd = udp;
            entry.events = POLLIN;
            entries.push_back(entry);
            owners.push_back({0, 0});
            for (int i = 0; i < 2; ++i) {
                if (listeners[i] == kNoSocket) continue;
                entry.fd = listeners[i];
                entry.events = POLLIN;
                entries.push_back(entry);
                owners.push_back({0, static_cast<std::uint32_t>(i)});
            }
            const std::size_t first_stream = entries.size();
            for (auto &[pid, peer] : peers) {
                for (auto &[sid, stream] : peer.streams) {
                    if (stream.tcp == kNoSocket) continue;
                    entry.fd = stream.tcp;
                    entry.events = 0;
                    if (stream.connecting || !stream.to_tcp.empty()) entry.events |= POLLOUT;
                    if (!stream.connecting && !stream.tcp_eof && stream.pending.size() < kReadAhead)
                        entry.events |= POLLIN;
                    entries.push_back(entry);
                    owners.push_back({pid, sid});
                }
            }
            poll_sockets(entries.data(), entries.size(), static_cast<int>(kTick.count()));
            if ((entries[0].revents & POLLIN) != 0) receive_all();
            for (std::size_t i = 1; i < first_stream; ++i)
                if ((entries[i].revents & POLLIN) != 0) accept_local(static_cast<int>(owners[i].stream));
            // Every stream, polled or not (data may have arrived for it).
            for (auto &[pid, peer] : peers) {
                for (auto it = peer.streams.begin(); it != peer.streams.end();) {
                    short revents = 0;
                    for (std::size_t i = first_stream; i < entries.size(); ++i)
                        if (owners[i].peer == pid && owners[i].stream == it->first) revents = entries[i].revents;
                    if (service_tcp(peer, it->first, it->second, revents)) {
                        ++it;
                        continue;
                    }
                    if (it->second.tcp != kNoSocket) close_hard(it->second.tcp);
                    if (it->second.ack_due) peer.final_acks.emplace_back(it->first, it->second.next_expected);
                    peer.closed.insert(it->first);
                    it = peer.streams.erase(it);
                }
            }
        }
        say_bye();
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() { stop(); }

void Session::set_log(std::function<void(const std::string &)> sink) {
    std::lock_guard lock(g_log_mutex);
    g_log_sink = std::move(sink);
}

bool Session::start(const SessionConfig &config) {
    stop();
    Impl &s = *impl_;
    auto fail = [&s](const std::string &why) {
        log("cannot start: " + why);
        std::lock_guard lock(s.mutex);
        s.snapshot = SessionStatus{};
        s.snapshot.error = why;
        return false;
    };
    if (!crypto_ready()) return fail("the encryption library did not start");
    s.config = config;
    s.secrets = config.secrets.empty() || config.role == Role::Join ? std::vector<Secret>{config.secret} : config.secrets;
    s.auths.clear();
    for (const Secret &secret : s.secrets) s.auths.push_back(auth_key(secret));
    s.auth = s.auths.front();
    s.peers.clear();
    s.refused = false;
    s.refusal.clear();
    s.bytes_in = s.bytes_out = 0;
    {
        std::lock_guard lock(s.mutex);
        s.snapshot = SessionStatus{};
        s.extra_candidates.clear();
        s.punch_targets.clear();
        s.raw_out.clear();
    }
    if (!s.open_udp(config.udp_port, config.role == Role::Host || config.udp_port != 0u))
        return fail("no UDP port free from " + std::to_string(config.udp_port));
    if (config.role == Role::Join) {
        if (!s.open_listeners()) {
            close_socket(s.udp);
            s.udp = kNoSocket;
            return fail("no local TCP ports free for the game's connection");
        }
        s.new_hello();
    }
    {
        std::lock_guard lock(s.mutex);
        s.snapshot.running = true;
        s.snapshot.udp_port = s.udp_port();
    }
    s.quit = false;
    s.thread = std::thread([&s] { s.run(); });
    log(std::string(config.role == Role::Host ? "hosting" : "joining") + " on UDP port " +
        std::to_string(s.udp_port()));
    return true;
}

void Session::stop() {
    Impl &s = *impl_;
    if (s.thread.joinable()) {
        s.quit = true;
        s.thread.join();
        log("stopped");
    }
    for (Socket &l : s.listeners) {
        if (l != kNoSocket) close_socket(l);
        l = kNoSocket;
    }
    if (s.udp != kNoSocket) close_socket(s.udp);
    s.udp = kNoSocket;
    s.hello.clear();
    s.hello_keys.reset();
    std::lock_guard lock(s.mutex);
    s.snapshot.running = false;
    s.snapshot.connected = false;
    s.snapshot.peers.clear();
}

SessionStatus Session::status() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot;
}

void Session::send_raw(const Endpoint &to, std::vector<std::uint8_t> data) {
    std::lock_guard lock(impl_->mutex);
    impl_->raw_out.emplace_back(to, std::move(data));
}

void Session::set_raw_handler(RawHandler handler) {
    std::lock_guard lock(impl_->mutex);
    impl_->raw_handler = std::move(handler);
}

void Session::punch(const std::vector<Endpoint> &targets) {
    std::lock_guard lock(impl_->mutex);
    impl_->punch_targets = targets;
    impl_->punch_until = Clock::now() + kPunchFor;
    impl_->last_punch = Clock::time_point{};
    log("opening the way to " + std::to_string(targets.size()) + " address(es) from a reply code");
}

void Session::add_candidates(const std::vector<Endpoint> &more) {
    std::lock_guard lock(impl_->mutex);
    for (const Endpoint &e : more)
        if (std::find(impl_->extra_candidates.begin(), impl_->extra_candidates.end(), e) ==
            impl_->extra_candidates.end())
            impl_->extra_candidates.push_back(e);
}

} // namespace portablekit::vc2::online
