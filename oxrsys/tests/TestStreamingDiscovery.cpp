// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "StreamingDiscovery.h"

using oxrsys::streaming_discovery::AnnounceTargets;
using oxrsys::streaming_discovery::InterfaceAddress;

namespace
{

constexpr uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return (a << 24) | (b << 16) | (c << 8) | d;
}

constexpr uint32_t Mask(uint32_t prefix)
{
    return prefix == 0 ? 0u : ~((1u << (32 - prefix)) - 1u);
}

} // namespace

TEST_CASE("The announce reaches the Steam Frame's dongle as well as the router's subnet", "[streaming][discovery]")
{
    // This PC on 2026-10-11: Ethernet to the router, the Steam Frame dongle, Tailscale, a Hyper-V switch.
    const std::vector<InterfaceAddress> adapters = {
        {Ip(192, 168, 1, 230), Mask(24)},
        {Ip(10, 35, 78, 23), Mask(24)},
        {Ip(100, 86, 241, 78), Mask(32)},
        {Ip(172, 22, 176, 1), Mask(20)},
    };
    const std::vector<uint32_t> targets = AnnounceTargets(adapters);
    const std::vector<uint32_t> expected = {Ip(192, 168, 1, 255), Ip(10, 35, 78, 255), Ip(172, 22, 191, 255),
                                            0xFFFFFFFFu};
    CHECK(targets == expected);
}

TEST_CASE("Adapters with no subnet to announce to are skipped", "[streaming][discovery]")
{
    const std::vector<InterfaceAddress> adapters = {
        {Ip(127, 0, 0, 1), Mask(8)},
        {Ip(169, 254, 3, 4), Mask(16)},
        {Ip(10, 0, 0, 1), Mask(31)},
        {Ip(10, 0, 0, 9), Mask(0)},
        {Ip(192, 168, 1, 230), Mask(24)},
        {Ip(192, 168, 1, 231), Mask(24)}, // a second address on the same subnet
    };
    const std::vector<uint32_t> targets = AnnounceTargets(adapters);
    const std::vector<uint32_t> expected = {Ip(192, 168, 1, 255), 0xFFFFFFFFu};
    CHECK(targets == expected);

    // Control: with no adapter found, the limited broadcast still goes out, as before.
    CHECK(AnnounceTargets({}) == std::vector<uint32_t>{0xFFFFFFFFu});
}
