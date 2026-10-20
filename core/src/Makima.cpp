// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Ported from Godot's Animation::makima_interpolate_in_time, Animation::spherical_makima_interpolate_in_time
// and Quaternion::blend_log (MIT), on plain arrays. The scalar curve is the one MATLAB's makima
// documents; the quaternion curve runs it in the log map of each end and blends the two.

#include "xrpilot/Makima.h"

#include <algorithm>
#include <cmath>

namespace xrpilot
{

namespace
{

constexpr double Epsilon = 1e-5;
constexpr double Pi = 3.14159265358979323846;

double akimaExtrapolate(double edge, double edgeNext)
{
    return 2.0 * edge - edgeNext;
}

double makimaTangent(double p0, double p1, double p2, double p3)
{
    const double w1 = std::fabs(p3 - p2) + std::fabs(p3 + p2) * 0.5;
    const double w2 = std::fabs(p1 - p0) + std::fabs(p1 + p0) * 0.5;
    if (w1 + w2 == 0.0)
        return (p1 + p2) * 0.5;
    return (w1 * p1 + w2 * p2) / (w1 + w2);
}

// Godot's signature: from at 0, to at toT, pre at preT (<= 0), post at postT, and the outer two.
double makimaInTime(double from, double to, double pre, double post, double prePre, double postPost, double weight,
                    double toT, double preT, double postT, double prePreT, double postPostT)
{
    const bool toCoincident = toT == 0.0;
    const bool preCoincident = preT == 0.0;
    const bool postCoincident = postT - toT == 0.0;
    const bool prePreCoincident = preT - prePreT == 0.0;
    const bool postPostCoincident = postPostT - postT == 0.0;

    const double mFrom = toCoincident ? 0.0 : (to - from) / toT;
    double mPre = preCoincident ? 0.0 : (from - pre) / -preT;
    double mPost = postCoincident ? 0.0 : (post - to) / (postT - toT);
    if (preCoincident)
        mPre = postCoincident ? mFrom : akimaExtrapolate(mFrom, mPost);
    if (postCoincident)
        mPost = preCoincident ? mFrom : akimaExtrapolate(mFrom, mPre);
    const double mPrePre = prePreCoincident ? akimaExtrapolate(mPre, mFrom) : (pre - prePre) / (preT - prePreT);
    const double mPostPost =
        postPostCoincident ? akimaExtrapolate(mPost, mFrom) : (postPost - post) / (postPostT - postT);
    const double tFrom = makimaTangent(mPrePre, mPre, mFrom, mPost);
    const double tTo = makimaTangent(mPre, mFrom, mPost, mPostPost);
    const double s2 = weight * weight;
    const double s3 = s2 * weight;
    return (2.0 * s3 - 3.0 * s2 + 1.0) * from + (s3 - 2.0 * s2 + weight) * toT * tFrom + (-2.0 * s3 + 3.0 * s2) * to +
           (s3 - s2) * toT * tTo;
}

struct Quat
{
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};

Quat fromArray(const float q[4])
{
    Quat out{q[0], q[1], q[2], q[3]};
    const double n = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
    if (n > 1e-12)
    {
        out.x /= n;
        out.y /= n;
        out.z /= n;
        out.w /= n;
    }
    return out;
}

double dot(const Quat& a, const Quat& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

Quat neg(const Quat& q)
{
    return {-q.x, -q.y, -q.z, -q.w};
}

Quat inverse(const Quat& q)
{
    return {-q.x, -q.y, -q.z, q.w};
}

Quat mul(const Quat& a, const Quat& b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Quaternion::log: axis times the full angle, as a pure quaternion.
Quat logQ(const Quat& q)
{
    if (std::fabs(q.w) > 1.0 - Epsilon)
        return {q.x * 2.0, q.y * 2.0, q.z * 2.0, 0.0};
    const double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    const double angle = 2.0 * std::atan2(s, q.w);
    return {q.x / s * angle, q.y / s * angle, q.z / s * angle, 0.0};
}

Quat expQ(const Quat& v)
{
    const double theta = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (theta < Epsilon)
        return {0.0, 0.0, 0.0, 1.0};
    const double s = std::sin(theta * 0.5) / theta;
    return {v.x * s, v.y * s, v.z * s, std::cos(theta * 0.5)};
}

// Godot's get_nearest_log: the log of q in the 720-degree phase nearest the neighbour's.
Quat nearestLog(const Quat& q, const Quat& neighbour)
{
    constexpr double Phase = 4.0 * Pi;
    double ax = q.x, ay = q.y, az = q.z;
    const double len = std::sqrt(ax * ax + ay * ay + az * az);
    const double angle = 2.0 * std::atan2(len, q.w);
    if (len < Epsilon)
    {
        const double nl = std::sqrt(neighbour.x * neighbour.x + neighbour.y * neighbour.y + neighbour.z * neighbour.z);
        if (nl < Epsilon)
            return {0.0, 0.0, 0.0, 0.0};
        ax = neighbour.x / nl;
        ay = neighbour.y / nl;
        az = neighbour.z / nl;
    }
    else
    {
        ax /= len;
        ay /= len;
        az /= len;
    }
    const double projected = neighbour.x * ax + neighbour.y * ay + neighbour.z * az;
    const double phases = std::round((projected - angle) / Phase);
    const double a = angle + phases * Phase;
    return {ax * a, ay * a, az * a, 0.0};
}

Quat makimaLog(const Quat& from, const Quat& to, const Quat& pre, const Quat& post, const Quat& prePre,
               const Quat& postPost, double weight, double toT, double preT, double postT, double prePreT,
               double postPostT)
{
    Quat out;
    out.x = makimaInTime(from.x, to.x, pre.x, post.x, prePre.x, postPost.x, weight, toT, preT, postT, prePreT, postPostT);
    out.y = makimaInTime(from.y, to.y, pre.y, post.y, prePre.y, postPost.y, weight, toT, preT, postT, prePreT, postPostT);
    out.z = makimaInTime(from.z, to.z, pre.z, post.z, prePre.z, postPost.z, weight, toT, preT, postT, prePreT, postPostT);
    out.w = 0.0;
    return out;
}

// Quaternion::blend_log: the to-space log is brought into from's log map by the inverse right
// Jacobian approximation, then the two are blended and mapped back.
Quat blendLog(const Quat& fromQ, const Quat& fromLn, const Quat& toLnIn, const Quat& toInFromLn, double weight)
{
    double tx = toLnIn.x, ty = toLnIn.y, tz = toLnIn.z;
    const double ux = toInFromLn.x, uy = toInFromLn.y, uz = toInFromLn.z;
    const double theta = std::sqrt(ux * ux + uy * uy + uz * uz);
    const double cx = uy * tz - uz * ty, cy = uz * tx - ux * tz, cz = ux * ty - uy * tx;
    tx += ux + cx * 0.5;
    ty += uy + cy * 0.5;
    tz += uz + cz * 0.5;
    if (theta >= Epsilon)
    {
        double c = 1.0 / (theta * theta);
        if (std::fabs(theta - Pi) > Epsilon)
            c -= (1.0 + std::cos(theta)) / (2.0 * theta * std::sin(theta));
        const double ccx = uy * cz - uz * cy, ccy = uz * cx - ux * cz, ccz = ux * cy - uy * cx;
        tx += ccx * c;
        ty += ccy * c;
        tz += ccz * c;
    }
    const Quat blended{fromLn.x + (tx - fromLn.x) * weight, fromLn.y + (ty - fromLn.y) * weight,
                       fromLn.z + (tz - fromLn.z) * weight, 0.0};
    return mul(fromQ, expQ(blended));
}

} // namespace

double makima(const double v[6], const double t[6], double weight)
{
    const double t0 = t[2];
    return makimaInTime(v[2], v[3], v[1], v[4], v[0], v[5], weight, t[3] - t0, t[1] - t0, t[4] - t0, t[0] - t0,
                        t[5] - t0);
}

void sphericalMakima(const float q[6][4], const double t[6], double weight, float out[4])
{
    Quat prePre = fromArray(q[0]), pre = fromArray(q[1]), from = fromArray(q[2]), to = fromArray(q[3]),
         post = fromArray(q[4]), postPost = fromArray(q[5]);

    // Flip to the shortest path, from the inner samples outward.
    const bool flip1 = std::signbit(dot(from, pre));
    pre = flip1 ? neg(pre) : pre;
    const bool flip0 = flip1 ? dot(pre, prePre) <= 0.0 : std::signbit(dot(pre, prePre));
    prePre = flip0 ? neg(prePre) : prePre;
    const bool flip2 = std::signbit(dot(from, to));
    to = flip2 ? neg(to) : to;
    const bool flip3 = flip2 ? dot(to, post) <= 0.0 : std::signbit(dot(to, post));
    post = flip3 ? neg(post) : post;
    const bool flip4 = flip3 ? dot(post, postPost) <= 0.0 : std::signbit(dot(post, postPost));
    postPost = flip4 ? neg(postPost) : postPost;

    const double t0 = t[2];
    const double toT = t[3] - t0, preT = t[1] - t0, postT = t[4] - t0, prePreT = t[0] - t0, postPostT = t[5] - t0;

    // In from's log map.
    const Quat fromInv = inverse(from);
    Quat lnFrom{0.0, 0.0, 0.0, 0.0};
    Quat lnTo = logQ(mul(fromInv, to));
    Quat lnPre = logQ(mul(fromInv, pre));
    Quat lnPost = logQ(mul(fromInv, post));
    Quat lnPrePre = nearestLog(mul(fromInv, prePre), lnPre);
    Quat lnPostPost = nearestLog(mul(fromInv, postPost), lnPost);
    const Quat fromLn = makimaLog(lnFrom, lnTo, lnPre, lnPost, lnPrePre, lnPostPost, weight, toT, preT, postT, prePreT, postPostT);
    const Quat toInFromLn = lnTo;

    // In to's log map.
    const Quat toInv = inverse(to);
    lnFrom = logQ(mul(toInv, from));
    lnTo = {0.0, 0.0, 0.0, 0.0};
    lnPre = logQ(mul(toInv, pre));
    lnPost = logQ(mul(toInv, post));
    lnPrePre = nearestLog(mul(toInv, prePre), lnPre);
    lnPostPost = nearestLog(mul(toInv, postPost), lnPost);
    const Quat toLn = makimaLog(lnFrom, lnTo, lnPre, lnPost, lnPrePre, lnPostPost, weight, toT, preT, postT, prePreT, postPostT);

    const Quat r = blendLog(from, fromLn, toLn, toInFromLn, weight);
    const double n = std::sqrt(dot(r, r));
    out[0] = float(r.x / n);
    out[1] = float(r.y / n);
    out[2] = float(r.z / n);
    out[3] = float(r.w / n);
}

void samplePath(const Pose* poses, const double* times, std::size_t n, double at, Pose& out)
{
    if (n == 0)
        return;
    if (n == 1 || at <= times[0])
    {
        out = poses[0];
        return;
    }
    if (at >= times[n - 1])
    {
        out = poses[n - 1];
        return;
    }
    std::size_t i = 0;
    while (i + 2 < n && times[i + 1] <= at)
        ++i;
    // Six samples around the segment [i, i + 1], ends repeated where the path runs out.
    const auto clampIndex = [n](long k) { return std::size_t(std::clamp<long>(k, 0, long(n) - 1)); };
    std::size_t idx[6];
    for (int k = 0; k < 6; ++k)
        idx[k] = clampIndex(long(i) + k - 2);
    double t[6];
    for (int k = 0; k < 6; ++k)
        t[k] = times[idx[k]];
    const double span = t[3] - t[2];
    const double weight = span > 0.0 ? std::clamp((at - t[2]) / span, 0.0, 1.0) : 1.0;
    for (int c = 0; c < 3; ++c)
    {
        double v[6];
        for (int k = 0; k < 6; ++k)
            v[k] = poses[idx[k]].position[c];
        out.position[c] = float(makima(v, t, weight));
    }
    float q[6][4];
    for (int k = 0; k < 6; ++k)
        toQuaternion(poses[idx[k]].rotation, q[k]);
    float r[4];
    sphericalMakima(q, t, weight, r);
    if (!fromQuaternion(r, out.rotation))
        out.rotation = poses[i].rotation;
}

} // namespace xrpilot
