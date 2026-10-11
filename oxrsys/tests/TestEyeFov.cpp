// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EyeFov.h"

using Catch::Approx;
using namespace oxrsys::eye_fov;

namespace
{

// The Steam Frame's left eye, as its client declared it on 2026-10-11.
const Angles frame{-1.03f, 0.88f, 0.86f, -1.05f};

} // namespace

TEST_CASE("The default renders the 90 degree square the driver always had", "[eyefov]")
{
    const ProjectionRaw raw = ToProjectionRaw(ToTangents(kDefault));
    CHECK(raw.left == Approx(-1.0f));
    CHECK(raw.right == Approx(1.0f));
    CHECK(raw.top == Approx(-1.0f));
    CHECK(raw.bottom == Approx(1.0f));
    CHECK(EyeWidth(ToTangents(kDefault), 1680) == 1680);
}

TEST_CASE("The Steam Frame's lopsided view projects as it was declared", "[eyefov]")
{
    const Tangents left = ToTangents(frame);
    const ProjectionRaw raw = ToProjectionRaw(left);
    CHECK(raw.left == Approx(-1.6652f).epsilon(0.001));
    CHECK(raw.right == Approx(1.2097f).epsilon(0.001));
    CHECK(raw.top == Approx(-1.1616f).epsilon(0.001));
    CHECK(raw.bottom == Approx(1.7433f).epsilon(0.001));

    // The right eye is its mirror: the wide side faces out.
    const ProjectionRaw right = ToProjectionRaw(Mirror(left));
    CHECK(right.left == Approx(-1.2097f).epsilon(0.001));
    CHECK(right.right == Approx(1.6652f).epsilon(0.001));
    CHECK(right.top == raw.top);

    // Nearly square: the 0.9-wide eye of the fixed driver squashed it.
    CHECK(EyeWidth(left, 1680) == 1664);
}

TEST_CASE("The settings text reads back, and a broken one is refused", "[eyefov]")
{
    Angles read;
    REQUIRE(Parse(Format(frame).c_str(), &read));
    CHECK_FALSE(Differs(read, frame));
    CHECK(Differs(frame, kDefault));

    Angles untouched = kDefault;
    CHECK_FALSE(Parse("", &untouched));
    CHECK_FALSE(Parse("1,2,3", &untouched));
    CHECK_FALSE(Parse("0.5,0.9,0.8,-0.9", &untouched)); // left edge on the right of the centre
    CHECK_FALSE(Parse("-1.6,0.9,0.8,-0.9", &untouched)); // past 85 degrees
    CHECK_FALSE(Differs(untouched, kDefault));
}

TEST_CASE("A client that sends no field of view is not one to use", "[eyefov]")
{
    const float none[4] = {};
    CHECK_FALSE(Valid(FromPacket(none)));
    const float sent[4] = {frame.left, frame.right, frame.up, frame.down};
    CHECK(Valid(FromPacket(sent)));
}

TEST_CASE("A headset's own eye height is saved once for the next start", "[eyefov]")
{
    // The Steam Frame's compositor recommends more than the 1680 the driver starts with.
    CHECK(HeightToSave(2160, 1680, 0) == 2160);
    CHECK(HeightToSave(2160, 1680, 2160) == 0); // already saved
    CHECK(HeightToSave(2160, 2160, 0) == 0);    // already rendered
    CHECK(HeightToSave(0, 1680, 0) == 0);       // a client that does not say
    CHECK(HeightToSave(8192, 1680, 0) == 0);    // past what the driver renders
    CHECK(ValidHeight(kMinEyeHeight));
    CHECK(ValidHeight(kMaxEyeHeight));
    CHECK_FALSE(ValidHeight(kMinEyeHeight - 1));
}
