// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "StreamingSpectators.h"

#include <cstring>

using namespace std::chrono_literals;
using oxrsys::streaming_spectators::Address;
using oxrsys::streaming_spectators::IsSessionOwner;
using oxrsys::streaming_spectators::List;
using oxrsys::streaming_spectators::MergeCopilot;
using namespace oxr::protocol;

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

// The desk's pilot holds both controllers, the trigger and A pressed on the right.
TrackingPacket DeskCopilot()
{
    TrackingPacket desk = {};
    desk.trackingFlags = TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE | TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE;
    desk.headPosition[1] = 1.2f;
    desk.leftControllerPos[0] = -0.3f;
    desk.rightControllerPos[0] = 0.3f;
    desk.rightControllerRot[3] = 1.0f;
    desk.rightTrigger = 1.0f;
    desk.buttonState = BUTTON_A | BUTTON_RIGHT_TRIGGER | BUTTON_X;
    return desk;
}

} // namespace

TEST_CASE("A co-pilot's controller fills a hand the headset is not using", "[streaming][spectator]")
{
    // The Steam Frame holds only its left controller.
    TrackingPacket frame = {};
    frame.trackingFlags = TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE;
    frame.headPosition[1] = 1.7f;
    frame.leftControllerPos[0] = -0.2f;
    frame.buttonState = BUTTON_Y;

    const TrackingPacket merged = MergeCopilot(frame, DeskCopilot());
    CHECK(merged.headPosition[1] == 1.7f);
    CHECK(merged.leftControllerPos[0] == -0.2f);
    CHECK(merged.rightControllerPos[0] == 0.3f);
    CHECK(merged.rightTrigger == 1.0f);
    CHECK(merged.trackingFlags == (TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE | TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE));
    // The left side's buttons stay the headset's: Y held, the desk's X not.
    CHECK(merged.buttonState == (BUTTON_Y | BUTTON_A | BUTTON_RIGHT_TRIGGER));
}

TEST_CASE("A co-pilot never moves the head or a hand the headset is using", "[streaming][spectator]")
{
    // The Quest 3 tracks its right hand without a controller, and holds its left controller.
    TrackingPacket quest = {};
    quest.trackingFlags = TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE | TRACKING_FLAG_RIGHT_HAND_ACTIVE;
    quest.headPosition[1] = 1.6f;
    quest.buttonState = BUTTON_MENU;

    const TrackingPacket merged = MergeCopilot(quest, DeskCopilot());
    CHECK(std::memcmp(&merged, &quest, sizeof(quest)) == 0);

    // Control: a co-pilot holding nothing changes nothing on an idle headset.
    const TrackingPacket idle = {};
    const TrackingPacket untouched = MergeCopilot(idle, TrackingPacket{});
    CHECK(std::memcmp(&untouched, &idle, sizeof(idle)) == 0);
}
