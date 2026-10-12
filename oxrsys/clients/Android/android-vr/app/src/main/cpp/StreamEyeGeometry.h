// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cmath>

namespace oxrsys::client
{

// A field of view as OpenXR gives it: the angles in radians to the left, right, up and down edges, left and
// down negative.
struct Fov
{
    float left = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    float down = 0.0f;
};

// Each edge on its own side of the centre.
inline bool IsValid(const Fov& f)
{
    return std::isfinite(f.left) && std::isfinite(f.right) && std::isfinite(f.up) && std::isfinite(f.down) &&
           f.left < 0.0f && f.right > 0.0f && f.up > 0.0f && f.down < 0.0f;
}

// The announce's renderEyeTangents (left, right, up, down): the left eye's view a server renders whatever
// the client sends. All zero means the server renders the client's own eyeFov.
inline bool FovFromTangents(const float tangents[4], Fov* out)
{
    const Fov f{std::atan(tangents[0]), std::atan(tangents[1]), std::atan(tangents[2]), std::atan(tangents[3])};
    if (!IsValid(f))
        return false;
    *out = f;
    return true;
}

// The server renders the right eye as the left eye's mirror.
inline Fov Mirror(const Fov& f)
{
    return {-f.right, -f.left, f.up, f.down};
}

// The view to declare in a projection layer for one eye of a streamed frame: the one its pixels were rendered
// with (XrCompositionLayerProjectionView::fov). That is the server's fixed view when it announced one, else
// the left view this client sent; the right eye is the mirror in both cases, never the headset's own right view.
inline Fov DeclaredFov(bool serverFixed, const Fov& serverLeft, const Fov& sentLeft, int eye)
{
    const Fov& left = serverFixed ? serverLeft : sentLeft;
    return eye == 0 ? left : Mirror(left);
}

} // namespace oxrsys::client
