// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// ---------------------------------------------------------------------------
// redfish-ssdp — SSDP responder for the Redfish service (DSP0266 + DSP0270)
// ---------------------------------------------------------------------------
// Redfish clients that do not already know the service address discover it with
// an SSDP M-SEARCH to 239.255.255.250:1900. In UEFI that is
// EFI_REDFISH_DISCOVER_PROTOCOL / RedfishDiscoverDxe; the firmware multicasts
// an M-SEARCH over the freshly configured USB-RNDIS host interface and expects
// the BMC to answer with the canonical service-root URI.
//
// bmcweb implements no SSDP at all (it only carries the SSDP *schema* property
// in ManagerNetworkProtocol), and nothing else in meta-phosphor does either, so
// on stock OpenBMC that discovery step silently finds nothing. This daemon
// closes that gap.
//
// Scope: this answers M-SEARCH and announces on start/stop. It deliberately does
// NOT implement the UPnP device/service description XML -- Redfish's SSDP
// profile (DSP0266 "SSDP" clause) only requires the search response carrying an
// AL header pointing at the service root.
//
// Response format (DSP0266):
//     HTTP/1.1 200 OK
//     CACHE-CONTROL: max-age=<n>
//     ST: urn:dmtf-org:service:redfish-rest:1
//     USN: uuid:<service uuid>::urn:dmtf-org:service:redfish-rest:1
//     AL: https://<bmc ip>/redfish/v1/
//     EXT:
//
// The AL address is derived per-request from IP_PKTINFO's ipi_spec_dst -- the
// local address the M-SEARCH was actually delivered to. That matters here: the
// BMC has both a management NIC and the 169.254.0.17 host interface, and each
// requester must be told the address it can actually reach us on. Hardcoding one
// would hand host firmware the management IP (or vice versa).

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>

namespace
{

constexpr const char* ssdpMulticast = "239.255.255.250";
constexpr uint16_t ssdpPort = 1900;

// DSP0266: the Redfish SSDP search target.
constexpr const char* redfishST = "urn:dmtf-org:service:redfish-rest:1";
constexpr unsigned cacheMaxAge = 1800;

volatile std::sig_atomic_t running = 1;

void onSignal(int)
{
    running = 0;
}

// Case-insensitive substring search: SSDP header names are case-insensitive and
// real clients vary (EDK2 sends "ST:", some tools send "st:").
bool containsCI(std::string_view hay, std::string_view needle)
{
    if (needle.size() > hay.size())
    {
        return false;
    }
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != hay.end();
}

// The Redfish service UUID, so USN matches what bmcweb reports in ServiceRoot.
// bmcweb reads the same property; if it is unavailable we fall back to a
// stable all-zero UUID rather than inventing one that would disagree with
// ServiceRoot on every restart.
std::string serviceUuid()
{
    try
    {
        auto bus = sdbusplus::bus::new_default();
        auto m = bus.new_method_call(
            "xyz.openbmc_project.Settings", "/xyz/openbmc_project/inventory/system",
            "org.freedesktop.DBus.Properties", "Get");
        m.append("xyz.openbmc_project.Common.UUID", "UUID");
        auto reply = bus.call(m);
        std::variant<std::string> v;
        reply.read(v);
        const auto& s = std::get<std::string>(v);
        if (!s.empty())
        {
            return s;
        }
    }
    catch (const std::exception&)
    {
        // Fall through -- discovery is still useful without a matching UUID.
    }
    return "00000000-0000-0000-0000-000000000000";
}

} // namespace

int main()
{
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    const std::string uuid = serviceUuid();

    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        std::fprintf(stderr, "redfish-ssdp: socket: %s\n", std::strerror(errno));
        return 1;
    }

    int one = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // Needed to learn which local address each M-SEARCH landed on.
    ::setsockopt(sock, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ssdpPort);
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::fprintf(stderr, "redfish-ssdp: bind 1900: %s\n",
                     std::strerror(errno));
        ::close(sock);
        return 1;
    }

    // Join the SSDP group on every interface (ifindex 0). New interfaces that
    // appear later -- usb0 is created by usb-network.service, which may well
    // start after us -- are picked up because the kernel applies an
    // INADDR_ANY membership to interfaces as they come up.
    ip_mreqn mreq{};
    ::inet_pton(AF_INET, ssdpMulticast, &mreq.imr_multiaddr);
    mreq.imr_address.s_addr = htonl(INADDR_ANY);
    mreq.imr_ifindex = 0;
    if (::setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                     sizeof(mreq)) < 0)
    {
        std::fprintf(stderr, "redfish-ssdp: join %s: %s\n", ssdpMulticast,
                     std::strerror(errno));
        // Not fatal: unicast M-SEARCH still works.
    }

    std::fprintf(stderr, "redfish-ssdp: listening on %s:%u, uuid %s\n",
                 ssdpMulticast, ssdpPort, uuid.c_str());

    std::array<char, 2048> buf{};
    while (running)
    {
        sockaddr_in from{};
        iovec iov{buf.data(), buf.size() - 1};
        std::array<char, CMSG_SPACE(sizeof(in_pktinfo))> control{};
        msghdr msg{};
        msg.msg_name = &from;
        msg.msg_namelen = sizeof(from);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.data();
        msg.msg_controllen = control.size();

        ssize_t n = ::recvmsg(sock, &msg, 0);
        if (n <= 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        buf[static_cast<size_t>(n)] = '\0';
        std::string_view req(buf.data(), static_cast<size_t>(n));

        if (!containsCI(req, "M-SEARCH"))
        {
            continue;
        }
        // Answer only searches we are actually a match for.
        if (!containsCI(req, redfishST) && !containsCI(req, "ssdp:all") &&
            !containsCI(req, "upnp:rootdevice"))
        {
            continue;
        }

        // Which of our addresses did this arrive on? That is what the requester
        // can reach us at.
        in_addr local{};
        for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm != nullptr;
             cm = CMSG_NXTHDR(&msg, cm))
        {
            if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO)
            {
                in_pktinfo pi{};
                std::memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
                local = pi.ipi_spec_dst;
                break;
            }
        }

        char localStr[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &local, localStr, sizeof(localStr));
        if (localStr[0] == '\0' || std::string_view(localStr) == "0.0.0.0")
        {
            continue;
        }

        char resp[512];
        int len = std::snprintf(
            resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "CACHE-CONTROL: max-age=%u\r\n"
            "ST: %s\r\n"
            "USN: uuid:%s::%s\r\n"
            "AL: https://%s/redfish/v1/\r\n"
            "EXT:\r\n"
            "\r\n",
            cacheMaxAge, redfishST, uuid.c_str(), redfishST, localStr);

        if (len > 0)
        {
            ::sendto(sock, resp, static_cast<size_t>(len), 0,
                     reinterpret_cast<sockaddr*>(&from), msg.msg_namelen);

            char fromStr[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &from.sin_addr, fromStr, sizeof(fromStr));
            std::fprintf(stderr, "redfish-ssdp: answered %s -> https://%s/redfish/v1/\n",
                         fromStr, localStr);
        }
    }

    ::close(sock);
    return 0;
}
