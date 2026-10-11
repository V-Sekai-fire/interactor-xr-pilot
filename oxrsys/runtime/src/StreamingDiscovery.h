// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace oxrsys::streaming_discovery
{

// An adapter's IPv4 address and netmask, host order.
struct InterfaceAddress
{
    uint32_t ip = 0;
    uint32_t mask = 0;
};

// Where the announce goes: each adapter's subnet broadcast, so a headset on any of them hears it (the
// limited broadcast 255.255.255.255 leaves by one adapter only), then the limited broadcast for a subnet
// the list missed. Loopback, link-local and point-to-point (/31, /32, as Tailscale's) adapters have no
// subnet to announce to. Host order, each once, in adapter order.
inline std::vector<uint32_t> AnnounceTargets(const std::vector<InterfaceAddress>& adapters)
{
    std::vector<uint32_t> targets;
    for (const InterfaceAddress& a : adapters)
    {
        const bool loopback = (a.ip >> 24) == 127;
        const bool linkLocal = (a.ip >> 16) == 0xA9FE; // 169.254/16
        const bool pointToPoint = a.mask == 0 || (~a.mask) < 2u;
        if (loopback || linkLocal || pointToPoint)
        {
            continue;
        }
        const uint32_t broadcast = a.ip | ~a.mask;
        if (std::find(targets.begin(), targets.end(), broadcast) == targets.end())
        {
            targets.push_back(broadcast);
        }
    }
    targets.push_back(0xFFFFFFFFu);
    return targets;
}

// This machine's IPv4 adapters that are up.
inline std::vector<InterfaceAddress> LocalAdapters()
{
    std::vector<InterfaceAddress> out;
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt)
    {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR)
    {
        return out;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp)
        {
            continue;
        }
        for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next)
        {
            const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            const uint8_t prefix = unicast->OnLinkPrefixLength;
            const uint32_t mask = prefix == 0 ? 0u : prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1u);
            out.push_back({ntohl(address->sin_addr.s_addr), mask});
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
    {
        return out;
    }
    for (ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next)
    {
        if (entry->ifa_addr == nullptr || entry->ifa_netmask == nullptr || entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_UP) == 0)
        {
            continue;
        }
        out.push_back({ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr),
                       ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask)->sin_addr.s_addr)});
    }
    freeifaddrs(list);
#endif
    return out;
}

} // namespace oxrsys::streaming_discovery
