// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace oxrsys::streaming_spectators
{

// A client's control address, IPv4 and port in host order.
struct Address
{
    uint32_t ip = 0;
    uint16_t port = 0;

    bool operator==(const Address& other) const { return ip == other.ip && port == other.port; }
};

// Clients that watch the session's video without owning it (CLIENT_CAPABILITY_SPECTATOR). Each repeats
// its ClientConnect as a heartbeat; one unheard for the timeout is dropped. Not thread-safe: the server
// holds its own lock around every call.
class List
{
public:
    using Clock = std::chrono::steady_clock;

    // Records a heartbeat; true when the spectator is new.
    bool Refresh(const Address& address, Clock::time_point now)
    {
        for (Entry& entry : entries_)
        {
            if (entry.address == address)
            {
                entry.lastSeen = now;
                return false;
            }
        }
        entries_.push_back({address, now});
        return true;
    }

    bool Remove(const Address& address)
    {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [&](const Entry& entry) { return entry.address == address; });
        if (it == entries_.end())
        {
            return false;
        }
        entries_.erase(it);
        return true;
    }

    bool Contains(const Address& address) const
    {
        return std::any_of(entries_.begin(), entries_.end(),
                           [&](const Entry& entry) { return entry.address == address; });
    }

    // Drops spectators last heard at or before now - timeout, and returns them.
    std::vector<Address> Expire(Clock::time_point now, std::chrono::milliseconds timeout)
    {
        std::vector<Address> expired;
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            if (now - it->lastSeen >= timeout)
            {
                expired.push_back(it->address);
                it = entries_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return expired;
    }

    std::vector<Address> Addresses() const
    {
        std::vector<Address> addresses;
        addresses.reserve(entries_.size());
        for (const Entry& entry : entries_)
        {
            addresses.push_back(entry.address);
        }
        return addresses;
    }

    size_t Size() const { return entries_.size(); }

private:
    struct Entry
    {
        Address address;
        Clock::time_point lastSeen;
    };
    std::vector<Entry> entries_;
};

// Whether a control message came from the client that owns the session: the same IP, and the same port
// when the owner's is known. A session with no owner has none.
inline bool IsSessionOwner(const std::string& ownerIp, uint16_t ownerPort, const std::string& fromIp,
                           uint16_t fromPort)
{
    return !ownerIp.empty() && ownerIp == fromIp && (ownerPort == 0 || ownerPort == fromPort);
}

} // namespace oxrsys::streaming_spectators
