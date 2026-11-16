#pragma once

// Finding the addresses an online session can be reached at, and opening the
// router for it. All of these block (for a few seconds at most) and are run
// from a helper thread, never the game's.
//
//   - this PC's own addresses (LAN IPv4, global IPv6), and its IPv4 gateway;
//   - UPnP IGD: SSDP discovery, the router's description, then
//     AddPortMapping / GetExternalIPAddress over SOAP; removed again when the
//     session ends;
//   - NAT-PMP (RFC 6886) to the gateway when UPnP finds nothing;
//   - STUN (RFC 5389) Binding requests sent from the session's own socket, so
//     the public address and port reported are the session's.
#include "online_invite.hpp"
#include "online_session.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace portablekit::vc2::online {

struct LocalAddresses {
    std::vector<Endpoint> lan;        // private and public IPv4, the port filled in
    std::vector<Endpoint> global_v6;  // global unicast IPv6
    std::optional<Endpoint> gateway;  // the default IPv4 gateway (port 0)
};
[[nodiscard]] LocalAddresses local_addresses(std::uint16_t port);

// A port mapping on the router, and the address outside it.
struct Mapping {
    std::string method;      // "UPnP" or "NAT-PMP"
    Endpoint external;       // the router's public address and the mapped port
    // For removal.
    std::string control_url; // UPnP
    std::string service;     // UPnP service type
    std::uint16_t internal_port{};
    std::optional<Endpoint> gateway;  // NAT-PMP
};

// Maps UDP `port` (the same port outside when the router allows it).
[[nodiscard]] std::optional<Mapping> map_port(std::uint16_t port, const LocalAddresses &local, std::string &log);
void unmap_port(const Mapping &mapping);

// The public address of the session's socket, from the STUN servers (host
// names resolved here). Empty when none answers in `timeout_ms`.
[[nodiscard]] std::optional<Endpoint> stun_public_address(Session &session, const std::vector<std::string> &servers,
                                                          int timeout_ms);

// The public STUN servers asked (Google, Cloudflare).
[[nodiscard]] const std::vector<std::string> &default_stun_servers();

// Every address `session`'s socket (on `port`) can be reached at, the most
// useful first: the router's mapping (when `router`; kept in `mapping` for
// unmap_port), the public address STUN saw (when `stun`; also on `port` itself
// for routers that keep the port), the LAN addresses, global IPv6. What it did
// goes to `log`.
[[nodiscard]] std::vector<Endpoint> gather_addresses(Session &session, std::uint16_t port, bool router, bool stun,
                                                     std::optional<Mapping> &mapping, std::string &log);

// For the tests: STUN packets.
[[nodiscard]] std::vector<std::uint8_t> stun_request(const std::array<std::uint8_t, 12> &transaction);
[[nodiscard]] std::optional<Endpoint> stun_parse(const std::uint8_t *data, std::size_t size,
                                                 const std::array<std::uint8_t, 12> &transaction);

// True for 10/8, 172.16/12, 192.168/16, 100.64/10 (carrier NAT) and 169.254/16.
[[nodiscard]] bool is_private_v4(const Endpoint &e);

} // namespace portablekit::vc2::online
