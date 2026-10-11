// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "StreamingSpectators.h"
#include "StreamModelCases.h"

#include <oxrsys/protocol/Spectator.h>

#include <string>

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

namespace
{

std::string Text(const std::vector<Address>& addresses)
{
    std::string out;
    for (const Address& a : addresses)
    {
        out += (out.empty() ? "" : ",") + std::to_string(a.ip) + ":" + std::to_string(a.port);
    }
    return out;
}

} // namespace

TEST_CASE("Control routing is the table lean/XrPilot/Stream.lean works out", "[streaming][spectator][model]")
{
    for (const stream_model::RouteCase& c : stream_model::routeCases)
    {
        INFO("packet " << static_cast<int>(c.packet) << " owner " << c.isOwner << " spectator " << c.isSpectator);
        CHECK(oxrsys::streaming_spectators::RouteControl(c.packet, c.isOwner, c.isSpectator) == c.route);
    }
}

TEST_CASE("The spectator list follows the runs lean/XrPilot/Stream.lean works out", "[streaming][spectator][model]")
{
    for (const stream_model::ListRun& run : stream_model::listRuns)
    {
        INFO(run.name);
        List spectators;
        size_t index = 0;
        for (const stream_model::ListOp& op : run.ops)
        {
            INFO("op " << index++);
            const List::Clock::time_point now{std::chrono::milliseconds(op.nowMs)};
            bool result = false;
            std::string dropped;
            switch (op.kind)
            {
            case stream_model::ListOpKind::Refresh:
                result = spectators.Refresh(op.address, now);
                break;
            case stream_model::ListOpKind::Remove:
                result = spectators.Remove(op.address);
                break;
            case stream_model::ListOpKind::Expire:
            {
                const std::vector<Address> expired =
                    spectators.Expire(now, std::chrono::milliseconds(stream_model::spectatorTimeoutMs));
                result = !expired.empty();
                dropped = Text(expired);
                break;
            }
            }
            CHECK(result == op.result);
            CHECK(dropped == op.dropped);
            CHECK(Text(spectators.Addresses()) == op.after);
        }
    }
}

TEST_CASE("A headset that finds the session owned watches it instead of taking it", "[streaming][spectators]")
{
    using oxrsys::streaming_spectators::ControlPacket;
    using oxrsys::streaming_spectators::ControlRoute;
    using oxrsys::streaming_spectators::RouteControl;
    namespace spectator = oxr::protocol::spectator;
    const uint32_t owned = oxr::protocol::SERVER_FEATURE_SESSION_OWNED | oxr::protocol::SERVER_FEATURE_CLIENT_UPSCALING;

    auto joinRoute = [](uint32_t features, bool usb) {
        return RouteControl(spectator::ShouldWatch(features, usb) ? ControlPacket::SpectatorConnect : ControlPacket::Connect,
                            false, false);
    };
    CHECK(joinRoute(owned, false) == ControlRoute::Watch);
    // Control: the same headset takes a session nobody owns, and a runtime too old to say takes it as before.
    CHECK(joinRoute(oxr::protocol::SERVER_FEATURE_CLIENT_UPSCALING, false) == ControlRoute::TakeSession);
    CHECK(joinRoute(0, false) == ControlRoute::TakeSession);
    // A USB cable is plugged in to drive, and spectators are sent video over UDP only.
    CHECK(joinRoute(owned, true) == ControlRoute::TakeSession);
}

TEST_CASE("A spectator headset's heartbeat keeps it inside the server's timeout", "[streaming][spectators]")
{
    namespace spectator = oxr::protocol::spectator;
    const int64_t t = 10'000'000'000;
    CHECK(spectator::HeartbeatDue(t, 0));
    CHECK_FALSE(spectator::HeartbeatDue(t + spectator::HeartbeatNs - 1, t));
    CHECK(spectator::HeartbeatDue(t + spectator::HeartbeatNs, t));
    CHECK(2 * spectator::HeartbeatNs < int64_t(oxr::protocol::SPECTATOR_TIMEOUT_MS) * 1'000'000);
}
