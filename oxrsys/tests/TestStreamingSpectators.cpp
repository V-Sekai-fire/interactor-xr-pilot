// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "StreamingSpectators.h"

using namespace std::chrono_literals;
using oxrsys::streaming_spectators::Address;
using oxrsys::streaming_spectators::IsSessionOwner;
using oxrsys::streaming_spectators::List;

namespace
{

const Address desk{0xC0A801E6u, 51000}; // 192.168.1.230, the desk's pilot
const Address second{0xC0A801E6u, 51001};

} // namespace

TEST_CASE("A spectator is added once and kept alive by its heartbeat", "[streaming][spectator]")
{
    List spectators;
    const List::Clock::time_point start{};
    CHECK(spectators.Refresh(desk, start));
    CHECK_FALSE(spectators.Refresh(desk, start + 1s));
    CHECK(spectators.Size() == 1);

    // Heard 1 s ago: still watching 4.9 s after the first heartbeat.
    CHECK(spectators.Expire(start + 4900ms, 5000ms).empty());
    CHECK(spectators.Contains(desk));
}

TEST_CASE("A spectator unheard for the timeout is dropped", "[streaming][spectator]")
{
    List spectators;
    const List::Clock::time_point start{};
    spectators.Refresh(desk, start);
    spectators.Refresh(second, start + 3s);

    const auto expired = spectators.Expire(start + 5s, 5000ms);
    REQUIRE(expired.size() == 1);
    CHECK(expired[0] == desk);
    CHECK_FALSE(spectators.Contains(desk));
    CHECK(spectators.Contains(second));
}

TEST_CASE("A spectator's goodbye removes only that spectator", "[streaming][spectator]")
{
    List spectators;
    spectators.Refresh(desk, {});
    spectators.Refresh(second, {});
    CHECK(spectators.Remove(desk));
    CHECK_FALSE(spectators.Remove(desk));
    CHECK(spectators.Addresses() == std::vector<Address>{second});
}

TEST_CASE("Only the session owner's address owns the session", "[streaming][spectator]")
{
    // The Steam Frame owns the session from 192.168.1.194:40438.
    CHECK(IsSessionOwner("192.168.1.194", 40438, "192.168.1.194", 40438));
    CHECK(IsSessionOwner("192.168.1.194", 0, "192.168.1.194", 1234));

    // Controls: the desk's pilot, the same headset from another socket, and a session with no owner.
    CHECK_FALSE(IsSessionOwner("192.168.1.194", 40438, "192.168.1.230", 40438));
    CHECK_FALSE(IsSessionOwner("192.168.1.194", 40438, "192.168.1.194", 40439));
    CHECK_FALSE(IsSessionOwner("", 0, "192.168.1.194", 40438));
}
