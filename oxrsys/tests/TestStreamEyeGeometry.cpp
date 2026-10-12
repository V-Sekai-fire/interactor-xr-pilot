// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "StreamEyeGeometry.h"

using Catch::Approx;
using namespace oxrsys::client;

namespace
{

// The Steam Frame's left eye as its client declared it on 2026-10-11 (TestEyeFov), and a right eye that is
// close to its mirror but not exact, as a headset's two eyes can be.
const Fov frameLeft{-1.03f, 0.88f, 0.86f, -1.05f};
const Fov frameRight{-0.87f, 1.04f, 0.86f, -1.05f};

// A SteamVR driver fixed at the 90 degree square it starts with.
const float squareTangents[4] = {-1.0f, 1.0f, 1.0f, -1.0f};

// Where a world direction (yaw from the eye's forward axis) lands across the image, 0 to 1.
float ImageX(const Fov& f, float yaw)
{
    const float l = std::tan(f.left);
    const float r = std::tan(f.right);
    return (std::tan(yaw) - l) / (r - l);
}

// The yaw at which the compositor shows an image column when the layer declares this view.
float ShownYaw(const Fov& f, float x)
{
    const float l = std::tan(f.left);
    const float r = std::tan(f.right);
    return std::atan(l + x * (r - l));
}

// Rendered with one view and shown with another: where a world direction 20 degrees off the axis is seen.
float Seen(const Fov& rendered, const Fov& declared, float yaw)
{
    return ShownYaw(declared, ImageX(rendered, yaw));
}

} // namespace

TEST_CASE("A layer declares the view the server rendered, so the world stays put", "[streameyes]")
{
    Fov server;
    REQUIRE(FovFromTangents(squareTangents, &server));
    const float yaw = 0.35f;
    for (int eye = 0; eye < 2; ++eye)
    {
        const Fov rendered = eye == 0 ? server : Mirror(server);
        const Fov declared = DeclaredFov(true, server, frameLeft, eye);
        CHECK(Seen(rendered, declared, yaw) == Approx(yaw).margin(1e-5));
    }
}

TEST_CASE("Declaring the headset's own view over a fixed server view moves the world", "[streameyes]")
{
    // The control: what the client did before. A direction 20 degrees off the axis is shown more than
    // 3 degrees away from where it is, and the error grows with the angle, so the world turns with the head.
    Fov server;
    REQUIRE(FovFromTangents(squareTangents, &server));
    const float yaw = 0.35f;
    CHECK(std::fabs(Seen(server, frameLeft, yaw) - yaw) > 0.05f);
    CHECK(std::fabs(Seen(Mirror(server), frameRight, yaw) - yaw) > 0.05f);
}

TEST_CASE("Without a fixed server view the right eye is the mirror of the left one sent", "[streameyes]")
{
    // The runtime renders the right eye as the sent left eye mirrored; the headset's own right view differs.
    const Fov rendered = Mirror(frameLeft);
    const Fov declared = DeclaredFov(false, Fov{}, frameLeft, 1);
    CHECK(Seen(rendered, declared, 0.35f) == Approx(0.35f).margin(1e-5));
    CHECK(std::fabs(Seen(rendered, frameRight, 0.35f) - 0.35f) > 0.005f);
    CHECK(DeclaredFov(false, Fov{}, frameLeft, 0).left == frameLeft.left);
}

TEST_CASE("No announced view means the server renders what the client sends", "[streameyes]")
{
    const float none[4] = {};
    Fov out = frameLeft;
    CHECK_FALSE(FovFromTangents(none, &out));
    CHECK(out.left == frameLeft.left);
    const float flipped[4] = {1.0f, -1.0f, 1.0f, -1.0f};
    CHECK_FALSE(FovFromTangents(flipped, &out));
}
