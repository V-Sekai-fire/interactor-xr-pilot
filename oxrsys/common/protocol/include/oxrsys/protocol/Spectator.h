// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

#include "Protocol.h"

namespace oxr::protocol::spectator
{

// As core/src/Client.cpp's SpectatorHeartbeatNs: one lost heartbeat in a row keeps the spectator
// (lean/XrPilot/Stream.lean, heartbeat_margin).
constexpr int64_t HeartbeatNs = 2'000'000'000;
static_assert(2 * HeartbeatNs < int64_t(SPECTATOR_TIMEOUT_MS) * 1'000'000, "a lost heartbeat must not drop it");

// A headset that finds another client owning the session watches instead of taking it. Spectators are
// sent video over UDP only, so a USB cable, which the user plugs in to drive, still takes the session.
inline bool ShouldWatch(uint32_t serverFeatures, bool usbTransport)
{
    return !usbTransport && (serverFeatures & SERVER_FEATURE_SESSION_OWNED) != 0;
}

inline bool HeartbeatDue(int64_t nowNs, int64_t lastHeartbeatNs)
{
    return lastHeartbeatNs == 0 || nowNs - lastHeartbeatNs >= HeartbeatNs;
}

} // namespace oxr::protocol::spectator
