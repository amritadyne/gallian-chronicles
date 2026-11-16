#include "online_nat.hpp"

#include "adhoc/sockets.hpp"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace portablekit::vc2::online {
namespace {

using namespace adhoc::net;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

Endpoint v4_endpoint(std::uint32_t network_order, std::uint16_t port) {
    Endpoint e;
    std::memcpy(e.ip.data(), &network_order, 4);
    e.port = port;
    return e;
}

std::uint32_t v4_of(const Endpoint &e) {
    std::uint32_t value = 0;
    std::memcpy(&value, e.ip.data(), 4);
    return value;
}

void add_v4(LocalAddresses &out, std::uint32_t network_order, std::uint16_t port) {
    const std::uint32_t host_order = ntohl(network_order);
    if ((host_order >> 24) == 127u || host_order == 0u || (host_order >> 16) == 0xA9FEu) return;  // loopback, link-local
    const Endpoint e = v4_endpoint(network_order, port);
    if (std::find(out.lan.begin(), out.lan.end(), e) == out.lan.end()) out.lan.push_back(e);
}

void add_v6(LocalAddresses &out, const std::uint8_t *bytes, std::uint16_t port) {
    if ((bytes[0] & 0xE0u) != 0x20u) return;  // global unicast only (2000::/3)
    Endpoint e;
    e.v6 = true;
    std::memcpy(e.ip.data(), bytes, 16);
    e.port = port;
    if (std::find(out.global_v6.begin(), out.global_v6.end(), e) == out.global_v6.end()) out.global_v6.push_back(e);
}

// A socket that gives up after `ms` on every send and receive.
void set_timeouts(Socket s, int ms) {
#if defined(_WIN32)
    const DWORD value = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&value), sizeof(value));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&value), sizeof(value));
#else
    timeval value{};
    value.tv_sec = ms / 1000;
    value.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&value), sizeof(value));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&value), sizeof(value));
#endif
}

// HTTP ------------------------------------------------------------------------

struct Url {
    std::string host;
    std::uint16_t port{80};
    std::string path{"/"};
};

std::optional<Url> parse_url(const std::string &text) {
    const std::string scheme = "http://";
    if (text.compare(0, scheme.size(), scheme) != 0) return std::nullopt;
    Url url;
    const std::size_t start = scheme.size();
    const std::size_t slash = text.find('/', start);
    std::string authority = text.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    url.path = slash == std::string::npos ? "/" : text.substr(slash);
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        url.host = authority.substr(0, colon);
        url.port = static_cast<std::uint16_t>(std::atoi(authority.c_str() + colon + 1));
    } else {
        url.host = authority;
    }
    if (url.host.empty() || url.port == 0u) return std::nullopt;
    return url;
}

// The router's addresses only: an IPv4 literal.
std::optional<Address> url_address(const Url &url) {
    in_addr address{};
    if (inet_pton(AF_INET, url.host.c_str(), &address) != 1) return std::nullopt;
    return Address::ipv4_address(address.s_addr, url.port);
}

// One request; the body of a 200 answer, and this PC's address on the way.
std::optional<std::string> http(const Url &url, const std::string &method, const std::string &headers,
                                const std::string &body, std::uint32_t *local_v4 = nullptr, int *status = nullptr) {
    const auto address = url_address(url);
    if (!address) return std::nullopt;
    Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) return std::nullopt;
    set_timeouts(s, 2500);
    if (::connect(s, reinterpret_cast<const sockaddr *>(&address->storage), address->length) != 0) {
        close_socket(s);
        return std::nullopt;
    }
    if (local_v4 != nullptr) {
        Address me;
        me.length = sizeof(me.storage);
        if (getsockname(s, reinterpret_cast<sockaddr *>(&me.storage), &me.length) == 0) *local_v4 = me.ipv4();
    }
    std::string request = method + " " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" + std::to_string(url.port) +
                          "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" +
                          headers + "\r\n" + body;
    std::size_t sent = 0;
    while (sent < request.size()) {
        const int n = ::send(s, request.data() + sent, static_cast<int>(request.size() - sent), kSendFlags);
        if (n <= 0) {
            close_socket(s);
            return std::nullopt;
        }
        sent += static_cast<std::size_t>(n);
    }
    std::string answer;
    char buffer[4096];
    while (answer.size() < 256u * 1024u) {
        const int n = ::recv(s, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        answer.append(buffer, static_cast<std::size_t>(n));
    }
    close_socket(s);
    const std::size_t space = answer.find(' ');
    if (space == std::string::npos) return std::nullopt;
    const int code = std::atoi(answer.c_str() + space + 1);
    if (status != nullptr) *status = code;
    const std::size_t split = answer.find("\r\n\r\n");
    if (split == std::string::npos) return std::nullopt;
    std::string content = answer.substr(split + 4);
    // Chunked answers (some routers): join the chunks.
    std::string head = answer.substr(0, split);
    std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (head.find("transfer-encoding: chunked") != std::string::npos) {
        std::string joined;
        std::size_t at = 0;
        while (at < content.size()) {
            const std::size_t eol = content.find("\r\n", at);
            if (eol == std::string::npos) break;
            const unsigned long size = std::strtoul(content.substr(at, eol - at).c_str(), nullptr, 16);
            if (size == 0u) break;
            joined += content.substr(eol + 2, size);
            at = eol + 2 + size + 2;
        }
        content = joined;
    }
    if (code != 200) return std::nullopt;
    return content;
}

// The text of the first <tag>…</tag> in `xml` from `from`, ignoring a
// namespace prefix on the tag.
std::optional<std::string> xml_value(const std::string &xml, const std::string &tag, std::size_t from = 0,
                                     std::size_t *after = nullptr) {
    std::size_t at = from;
    while (true) {
        at = xml.find(tag, at);
        if (at == std::string::npos) return std::nullopt;
        const std::size_t open = xml.rfind('<', at);
        const std::size_t close = xml.find('>', at);
        if (open == std::string::npos || close == std::string::npos) return std::nullopt;
        const std::string name = xml.substr(open + 1, at + tag.size() - open - 1);
        const bool plain = name == tag || (name.find(':') != std::string::npos && name.substr(name.find(':') + 1) == tag);
        if (!plain || (xml[at + tag.size()] != '>' && xml[at + tag.size()] != ' ')) {
            at += tag.size();
            continue;
        }
        const std::size_t end = xml.find("</", close);
        if (end == std::string::npos) return std::nullopt;
        if (after != nullptr) *after = end;
        return xml.substr(close + 1, end - close - 1);
    }
}

std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    return text.substr(start);
}

// UPnP --------------------------------------------------------------------------

std::vector<std::string> ssdp_locations(std::string &log) {
    std::vector<std::string> locations;
    Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return locations;
    set_nonblocking(s);
    set_option(s, IPPROTO_IP, IP_MULTICAST_TTL, 2);
    in_addr multicast{};
    inet_pton(AF_INET, "239.255.255.250", &multicast);
    const Address group = Address::ipv4_address(multicast.s_addr, 1900);
    const char *targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                             "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                             "urn:schemas-upnp-org:service:WANIPConnection:1"};
    for (int round = 0; round < 2; ++round) {
        for (const char *target : targets) {
            const std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                                                   "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") +
                                       target + "\r\n\r\n";
            ::sendto(s, search.data(), static_cast<int>(search.size()), 0,
                     reinterpret_cast<const sockaddr *>(&group.storage), group.length);
        }
        const auto until = Clock::now() + milliseconds(round == 0 ? 1200 : 800);
        while (Clock::now() < until) {
            PollEntry entry{};
            entry.fd = s;
            entry.events = POLLIN;
            poll_sockets(&entry, 1, 100);
            char buffer[2048];
            const int n = static_cast<int>(::recv(s, buffer, sizeof(buffer) - 1, 0));
            if (n <= 0) continue;
            std::string answer(buffer, static_cast<std::size_t>(n));
            std::string lower = answer;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::size_t at = lower.find("\nlocation:");
            if (at == std::string::npos) continue;
            const std::size_t end = answer.find('\r', at);
            const std::string location = trim(answer.substr(at + 10, end - at - 10));
            if (std::find(locations.begin(), locations.end(), location) == locations.end()) {
                locations.push_back(location);
                log += "UPnP: a router at " + location + "\n";
            }
        }
        if (!locations.empty()) break;
    }
    close_socket(s);
    return locations;
}

struct Igd {
    Url control;
    std::string service;
    std::uint32_t local_v4{};
};

std::optional<Igd> igd_from(const std::string &location) {
    const auto url = parse_url(location);
    if (!url) return std::nullopt;
    Igd igd;
    const auto description = http(*url, "GET", "", "", &igd.local_v4);
    if (!description) return std::nullopt;
    const char *services[] = {"urn:schemas-upnp-org:service:WANIPConnection:2",
                              "urn:schemas-upnp-org:service:WANIPConnection:1",
                              "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    for (const char *service : services) {
        std::size_t at = description->find(std::string(">") + service + "<");
        if (at == std::string::npos) continue;
        const auto control = xml_value(*description, "controlURL", at);
        if (!control) continue;
        std::string target = trim(*control);
        if (target.compare(0, 7, "http://") != 0) {
            std::string base = "http://" + url->host + ":" + std::to_string(url->port);
            if (const auto declared = xml_value(*description, "URLBase")) {
                base = trim(*declared);
                while (!base.empty() && base.back() == '/') base.pop_back();
            }
            target = base + (target.empty() || target[0] != '/' ? "/" : "") + target;
        }
        const auto control_url = parse_url(target);
        if (!control_url) continue;
        igd.control = *control_url;
        igd.service = service;
        return igd;
    }
    return std::nullopt;
}

std::optional<std::string> soap(const Igd &igd, const std::string &action, const std::string &arguments,
                                int *status = nullptr) {
    const std::string body = "<?xml version=\"1.0\"?>\r\n"
                             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                             "<u:" + action + " xmlns:u=\"" + igd.service + "\">" + arguments + "</u:" + action +
                             "></s:Body></s:Envelope>\r\n";
    const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + igd.service + "#" +
                                action + "\"\r\n";
    return http(igd.control, "POST", headers, body, nullptr, status);
}

std::string dotted(std::uint32_t network_order) {
    const auto *b = reinterpret_cast<const unsigned char *>(&network_order);
    return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." + std::to_string(b[3]);
}

std::optional<Mapping> upnp_map(std::uint16_t port, std::string &log) {
    for (const std::string &location : ssdp_locations(log)) {
        const auto igd = igd_from(location);
        if (!igd) {
            log += "UPnP: no WAN connection service at " + location + "\n";
            continue;
        }
        const auto external = soap(*igd, "GetExternalIPAddress", "");
        std::optional<std::string> ip;
        if (external) ip = xml_value(*external, "NewExternalIPAddress");
        in_addr parsed{};
        if (!ip || inet_pton(AF_INET, trim(*ip).c_str(), &parsed) != 1 || parsed.s_addr == 0u) {
            log += "UPnP: the router did not give its public address\n";
            continue;
        }
        // The same port outside if free, else a few others; a lease first
        // (some routers want one), then a permanent one (some refuse leases).
        for (int attempt = 0; attempt < 6; ++attempt) {
            const auto outside = static_cast<std::uint16_t>(port + (attempt / 2) * 7);
            const int lease = attempt % 2 == 0 ? 7200 : 0;
            const std::string arguments =
                "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(outside) +
                "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(port) +
                "</NewInternalPort><NewInternalClient>" + dotted(igd->local_v4) +
                "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>Valkyria Chronicles II"
                "</NewPortMappingDescription><NewLeaseDuration>" + std::to_string(lease) + "</NewLeaseDuration>";
            int status = 0;
            if (soap(*igd, "AddPortMapping", arguments, &status)) {
                Mapping mapping;
                mapping.method = "UPnP";
                mapping.external = v4_endpoint(parsed.s_addr, outside);
                mapping.control_url = "http://" + igd->control.host + ":" + std::to_string(igd->control.port) +
                                      igd->control.path;
                mapping.service = igd->service;
                mapping.internal_port = port;
                return mapping;
            }
            log += "UPnP: mapping port " + std::to_string(outside) + " refused (HTTP " + std::to_string(status) + ")\n";
        }
    }
    return std::nullopt;
}

// NAT-PMP --------------------------------------------------------------------------

std::optional<std::vector<std::uint8_t>> pmp_ask(const Endpoint &gateway, const std::vector<std::uint8_t> &request,
                                                  std::size_t answer_size) {
    Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return std::nullopt;
    set_nonblocking(s);
    const Address to = Address::ipv4_address(v4_of(gateway), 5351);
    // RFC 6886: 250 ms, doubling.
    int wait = 250;
    for (int attempt = 0; attempt < 4; ++attempt, wait *= 2) {
        ::sendto(s, reinterpret_cast<const char *>(request.data()), static_cast<int>(request.size()), 0,
                 reinterpret_cast<const sockaddr *>(&to.storage), to.length);
        const auto until = Clock::now() + milliseconds(wait);
        while (Clock::now() < until) {
            PollEntry entry{};
            entry.fd = s;
            entry.events = POLLIN;
            poll_sockets(&entry, 1, 50);
            std::uint8_t buffer[64];
            Address from;
            from.length = sizeof(from.storage);
            const int n = static_cast<int>(::recvfrom(s, reinterpret_cast<char *>(buffer), sizeof(buffer), 0,
                                                      reinterpret_cast<sockaddr *>(&from.storage), &from.length));
            if (n < static_cast<int>(answer_size) || from.ipv4() != v4_of(gateway)) continue;
            if (buffer[0] != 0u || buffer[1] != static_cast<std::uint8_t>(128u + request[1])) continue;
            close_socket(s);
            return std::vector<std::uint8_t>(buffer, buffer + n);
        }
    }
    close_socket(s);
    return std::nullopt;
}

std::optional<Mapping> pmp_map(std::uint16_t port, const Endpoint &gateway, std::string &log, std::uint32_t lifetime = 7200) {
    const auto address = pmp_ask(gateway, {0, 0}, 12);
    if (!address || (*address)[2] != 0u || (*address)[3] != 0u) {
        log += "NAT-PMP: no answer from the gateway " + gateway.describe() + "\n";
        return std::nullopt;
    }
    std::uint32_t ip = 0;
    std::memcpy(&ip, address->data() + 8, 4);
    std::vector<std::uint8_t> request{0, 1, 0, 0,
                                      static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port),
                                      static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port),
                                      static_cast<std::uint8_t>(lifetime >> 24), static_cast<std::uint8_t>(lifetime >> 16),
                                      static_cast<std::uint8_t>(lifetime >> 8), static_cast<std::uint8_t>(lifetime)};
    const auto mapped = pmp_ask(gateway, request, 16);
    if (!mapped || (*mapped)[2] != 0u || (*mapped)[3] != 0u) {
        log += "NAT-PMP: the gateway refused the mapping\n";
        return std::nullopt;
    }
    Mapping mapping;
    mapping.method = "NAT-PMP";
    mapping.external = v4_endpoint(ip, static_cast<std::uint16_t>(((*mapped)[10] << 8) | (*mapped)[11]));
    mapping.internal_port = port;
    mapping.gateway = gateway;
    return mapping;
}

// STUN ---------------------------------------------------------------------------

constexpr std::uint32_t kMagicCookie = 0x2112A442u;

std::vector<Endpoint> resolve_v4(const std::string &server) {
    std::vector<Endpoint> out;
    std::string host = server;
    std::uint16_t port = 3478;
    if (const std::size_t colon = server.rfind(':'); colon != std::string::npos) {
        host = server.substr(0, colon);
        port = static_cast<std::uint16_t>(std::atoi(server.c_str() + colon + 1));
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0) return out;
    for (addrinfo *at = found; at != nullptr && out.size() < 2u; at = at->ai_next) {
        const auto *in = reinterpret_cast<const sockaddr_in *>(at->ai_addr);
        out.push_back(v4_endpoint(in->sin_addr.s_addr, port));
    }
    freeaddrinfo(found);
    return out;
}

} // namespace

bool is_private_v4(const Endpoint &e) {
    if (e.v6) return false;
    const std::uint8_t a = e.ip[0], b = e.ip[1];
    return a == 10u || (a == 172u && (b & 0xF0u) == 16u) || (a == 192u && b == 168u) ||
           (a == 100u && (b & 0xC0u) == 64u) || (a == 169u && b == 254u);
}

LocalAddresses local_addresses(std::uint16_t port) {
    LocalAddresses out;
#if defined(_WIN32)
    ULONG size = 32u * 1024u;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_MULTICAST |
                                                     GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_ANYCAST,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
    }
    if (result != NO_ERROR) return out;
    for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            const sockaddr *sa = unicast->Address.lpSockaddr;
            if (sa->sa_family == AF_INET) add_v4(out, reinterpret_cast<const sockaddr_in *>(sa)->sin_addr.s_addr, port);
            else if (sa->sa_family == AF_INET6 && unicast->DadState == IpDadStatePreferred)
                add_v6(out, reinterpret_cast<const sockaddr_in6 *>(sa)->sin6_addr.s6_addr, port);
        }
        for (auto *gateway = adapter->FirstGatewayAddress; gateway != nullptr && !out.gateway; gateway = gateway->Next) {
            const sockaddr *sa = gateway->Address.lpSockaddr;
            if (sa->sa_family == AF_INET)
                out.gateway = v4_endpoint(reinterpret_cast<const sockaddr_in *>(sa)->sin_addr.s_addr, 0);
        }
    }
#else
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs *at = list; at != nullptr; at = at->ifa_next) {
            if (at->ifa_addr == nullptr || (at->ifa_flags & IFF_UP) == 0 || (at->ifa_flags & IFF_LOOPBACK) != 0) continue;
            if (at->ifa_addr->sa_family == AF_INET)
                add_v4(out, reinterpret_cast<const sockaddr_in *>(at->ifa_addr)->sin_addr.s_addr, port);
            else if (at->ifa_addr->sa_family == AF_INET6)
                add_v6(out, reinterpret_cast<const sockaddr_in6 *>(at->ifa_addr)->sin6_addr.s6_addr, port);
        }
        freeifaddrs(list);
    }
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line);
    while (std::getline(routes, line)) {
        std::istringstream fields(line);
        std::string name, destination, gateway;
        fields >> name >> destination >> gateway;
        if (destination == "00000000" && gateway != "00000000") {
            const auto value = static_cast<std::uint32_t>(std::strtoul(gateway.c_str(), nullptr, 16));
            out.gateway = v4_endpoint(value, 0);  // already in network order on little-endian
            break;
        }
    }
#endif
    // Private addresses first: those are the ones a LAN partner reaches.
    std::stable_sort(out.lan.begin(), out.lan.end(),
                     [](const Endpoint &a, const Endpoint &b) { return is_private_v4(a) && !is_private_v4(b); });
    return out;
}

std::optional<Mapping> map_port(std::uint16_t port, const LocalAddresses &local, std::string &log) {
    if (auto mapping = upnp_map(port, log)) return mapping;
    if (local.gateway) {
        if (auto mapping = pmp_map(port, *local.gateway, log)) return mapping;
    }
    return std::nullopt;
}

void unmap_port(const Mapping &mapping) {
    if (mapping.method == "UPnP") {
        Igd igd;
        const auto url = parse_url(mapping.control_url);
        if (!url) return;
        igd.control = *url;
        igd.service = mapping.service;
        soap(igd, "DeletePortMapping",
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(mapping.external.port) +
                 "</NewExternalPort><NewProtocol>UDP</NewProtocol>");
    } else if (mapping.method == "NAT-PMP" && mapping.gateway) {
        std::string ignored;
        (void)pmp_map(mapping.internal_port, *mapping.gateway, ignored, 0u);
    }
}

std::vector<std::uint8_t> stun_request(const std::array<std::uint8_t, 12> &transaction) {
    std::vector<std::uint8_t> packet{0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    packet.insert(packet.end(), transaction.begin(), transaction.end());
    return packet;
}

std::optional<Endpoint> stun_parse(const std::uint8_t *data, std::size_t size,
                                   const std::array<std::uint8_t, 12> &transaction) {
    if (size < 20u || data[0] != 0x01 || data[1] != 0x01) return std::nullopt;
    const std::uint32_t cookie = (std::uint32_t{data[4]} << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    if (cookie != kMagicCookie || std::memcmp(data + 8, transaction.data(), 12) != 0) return std::nullopt;
    const std::size_t length = static_cast<std::size_t>((data[2] << 8) | data[3]);
    if (20u + length > size) return std::nullopt;
    std::optional<Endpoint> plain;
    std::size_t at = 20u;
    while (at + 4u <= 20u + length) {
        const unsigned type = (data[at] << 8) | data[at + 1];
        const std::size_t attribute = static_cast<std::size_t>((data[at + 2] << 8) | data[at + 3]);
        const std::uint8_t *value = data + at + 4;
        if (at + 4u + attribute > 20u + length) break;
        if ((type == 0x0020u || type == 0x0001u) && attribute >= 8u && value[1] == 0x01u) {  // IPv4
            Endpoint e;
            std::uint16_t port = static_cast<std::uint16_t>((value[2] << 8) | value[3]);
            std::uint8_t ip[4] = {value[4], value[5], value[6], value[7]};
            if (type == 0x0020u) {
                port ^= static_cast<std::uint16_t>(kMagicCookie >> 16);
                for (int i = 0; i < 4; ++i) ip[i] ^= static_cast<std::uint8_t>(kMagicCookie >> (24 - 8 * i));
            }
            std::memcpy(e.ip.data(), ip, 4);
            e.port = port;
            if (type == 0x0020u) return e;
            plain = e;
        }
        at += 4u + ((attribute + 3u) & ~std::size_t{3});
    }
    return plain;
}

std::optional<Endpoint> stun_public_address(Session &session, const std::vector<std::string> &servers,
                                            int timeout_ms) {
    std::vector<Endpoint> targets;
    for (const std::string &server : servers)
        for (const Endpoint &e : resolve_v4(server)) targets.push_back(e);
    if (targets.empty()) return std::nullopt;
    std::array<std::uint8_t, 12> transaction{};
    random_bytes(transaction.data(), transaction.size());
    struct Shared {
        std::mutex mutex;
        std::condition_variable done;
        std::optional<Endpoint> result;
    };
    auto shared = std::make_shared<Shared>();
    session.set_raw_handler([shared, transaction](const Endpoint &, const std::uint8_t *data, std::size_t size) {
        if (auto found = stun_parse(data, size, transaction)) {
            std::lock_guard lock(shared->mutex);
            if (!shared->result) shared->result = found;
            shared->done.notify_all();
        }
    });
    const auto request = stun_request(transaction);
    const auto until = Clock::now() + milliseconds(timeout_ms);
    std::optional<Endpoint> result;
    for (int round = 0; Clock::now() < until; ++round) {
        for (const Endpoint &target : targets) session.send_raw(target, request);
        std::unique_lock lock(shared->mutex);
        shared->done.wait_for(lock, milliseconds(std::min(500 * (round + 1), 1500)), [&] { return shared->result.has_value(); });
        if (shared->result) {
            result = shared->result;
            break;
        }
    }
    session.set_raw_handler(nullptr);
    return result;
}

const std::vector<std::string> &default_stun_servers() {
    static const std::vector<std::string> servers = {"stun.l.google.com:19302", "stun.cloudflare.com:3478"};
    return servers;
}

std::vector<Endpoint> gather_addresses(Session &session, std::uint16_t port, bool router, bool stun,
                                       std::optional<Mapping> &mapping, std::string &log) {
    const LocalAddresses local = local_addresses(port);
    std::vector<Endpoint> out;
    if (router) {
        mapping = map_port(port, local, log);
        if (mapping) {
            log += "[vc2-online] " + mapping->method + ": the router forwards " + mapping->external.describe() +
                   " to this PC\n";
            out.push_back(mapping->external);
        } else {
            log += "[vc2-online] the router did not map a port (UPnP and NAT-PMP)\n";
        }
    }
    if (stun) {
        if (const auto seen = stun_public_address(session, default_stun_servers(), 3000)) {
            log += "[vc2-online] STUN: the internet sees this PC as " + seen->describe() + "\n";
            out.push_back(*seen);
            if (seen->port != port) {
                Endpoint same = *seen;
                same.port = port;
                out.push_back(same);
            }
        } else {
            log += "[vc2-online] STUN: no answer\n";
        }
    }
    for (const Endpoint &e : local.lan) out.push_back(e);
    for (const Endpoint &e : local.global_v6) out.push_back(e);
    std::vector<Endpoint> unique;
    for (const Endpoint &e : out)
        if (std::find(unique.begin(), unique.end(), e) == unique.end()) unique.push_back(e);
    return unique;
}

} // namespace portablekit::vc2::online
