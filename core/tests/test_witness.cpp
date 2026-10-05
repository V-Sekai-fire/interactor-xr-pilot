// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Plausible-witness-dag tests for the pilot's pure maths. Each invariant is a node: a property the
// production function should hold (the ladder searches for a falsifying witness and should find
// none) paired with an ablation that removes the safeguard and must surface a witness. The nodes
// form a dependency DAG; witnessdag.ablation-coverage enumerates it, so a node without a working
// control cannot pass as one. The ladder is third_party/witness-cpp (MIT), the C++ shape of
// V-Sekai-fire/plausible-witness-dag.

#include "xrpilot/Agent.h"
#include "xrpilot/Body.h"
#include "xrpilot/Rotation.h"

#include "witness/ladder.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace xrpilot;

namespace
{

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
        ++failures;
    }
}

// A fixed seed makes the ladder reproducible across runs; a real witness still prints its own seed.
constexpr uint64_t Seed = 0xD16E57ULL;
constexpr double Pi = 3.14159265358979323846;

bool noWitness(const witness::Trial& t)
{
    return t.outcome != witness::Outcome::FOUND;
}
bool isWitness(const witness::Trial& t)
{
    return t.outcome == witness::Outcome::FOUND;
}

struct Euler
{
    double a;
    double b;
    double c;
};
struct TwoEuler
{
    Euler first;
    Euler second;
};
struct Quat
{
    double x;
    double y;
    double z;
    double w;
};
struct Reach
{
    double yaw;
    double pitch;
    double dist;
};

Euler genEuler(witness::RNG& rng, const witness::Level&)
{
    return {rng.float_range(-179.0, 179.0), rng.float_range(-85.0, 85.0), rng.float_range(-179.0, 179.0)};
}

TwoEuler genTwoEuler(witness::RNG& rng, const witness::Level& lvl)
{
    return {genEuler(rng, lvl), genEuler(rng, lvl)};
}

Quat genQuat(witness::RNG& rng, const witness::Level&)
{
    return {rng.float_range(-2.0, 2.0), rng.float_range(-2.0, 2.0), rng.float_range(-2.0, 2.0),
            rng.float_range(-2.0, 2.0)};
}

witness::Generator<Reach> genReach(double low, double high)
{
    return [low, high](witness::RNG& rng, const witness::Level&) -> Reach {
        return {rng.float_range(-180.0, 180.0), rng.float_range(-80.0, 80.0), rng.float_range(low, high)};
    };
}

Rotation euler(const Euler& e)
{
    return fromEuler(EulerOrder::YXZ, float(e.a), float(e.b), float(e.c));
}

double worstEntry(const Rotation& a, const Rotation& b)
{
    double worst = 0.0;
    for (int i = 0; i < 9; ++i)
        worst = std::max(worst, double(std::abs(a.m[i] - b.m[i])));
    return worst;
}

double distance3(const float* p, const float* q)
{
    return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
}

double armReach()
{
    BodyModel body;
    return body.reach(1.6f);
}

void targetFromShoulder(const float shoulder[3], const Reach& r, float out[3])
{
    const double yaw = r.yaw * Pi / 180.0;
    const double pitch = r.pitch * Pi / 180.0;
    out[0] = shoulder[0] + float(std::cos(pitch) * std::sin(yaw) * r.dist);
    out[1] = shoulder[1] + float(std::sin(pitch) * r.dist);
    out[2] = shoulder[2] + float(-std::cos(pitch) * std::cos(yaw) * r.dist);
}

// The ablations: each removes one safeguard the matching property depends on.
Rotation sheared(Rotation r)
{
    r.m[1] += 0.5f;
    return r;
}
Rotation sumInsteadOfProduct(const Rotation& a, const Rotation& b)
{
    Rotation out;
    for (int i = 0; i < 9; ++i)
        out.m[i] = a.m[i] + b.m[i];
    return out;
}
Rotation quatWithoutNormalising(const Quat& q)
{
    const double x = q.x;
    const double y = q.y;
    const double z = q.z;
    const double w = q.w;
    Rotation r;
    r.m[0] = float(1 - 2 * (y * y + z * z));
    r.m[1] = float(2 * (x * y - z * w));
    r.m[2] = float(2 * (x * z + y * w));
    r.m[3] = float(2 * (x * y + z * w));
    r.m[4] = float(1 - 2 * (x * x + z * z));
    r.m[5] = float(2 * (y * z - x * w));
    r.m[6] = float(2 * (x * z - y * w));
    r.m[7] = float(2 * (y * z + x * w));
    r.m[8] = float(1 - 2 * (x * x + y * y));
    return r;
}

// -------- euler-orthonormal: fromEuler is always a rotation. --------
witness::Trial holdEulerOrtho()
{
    witness::Generator<Euler> gen = &genEuler;
    std::function<bool(const Euler&)> pred = [](const Euler& e) { return isRotation(euler(e)); };
    return witness::resolve<Euler>("fromEuler is orthonormal", gen, pred, Seed);
}
witness::Trial ablateEulerOrtho()
{
    witness::Generator<Euler> gen = &genEuler;
    std::function<bool(const Euler&)> pred = [](const Euler& e) { return isRotation(sheared(euler(e))); };
    return witness::resolve<Euler>("fromEuler orthonormal, shear ablated", gen, pred, Seed);
}

// -------- quaternion-orthonormal: fromQuaternion normalises, so its output is a rotation. --------
witness::Trial holdQuatOrtho()
{
    witness::Generator<Quat> gen = &genQuat;
    std::function<bool(const Quat&)> pred = [](const Quat& q) {
        const float raw[4] = {float(q.x), float(q.y), float(q.z), float(q.w)};
        Rotation r;
        if (!fromQuaternion(raw, r))
        {
            witness::assume(false);
            return true;
        }
        return isRotation(r);
    };
    return witness::resolve<Quat>("fromQuaternion is orthonormal", gen, pred, Seed);
}
witness::Trial ablateQuatOrtho()
{
    witness::Generator<Quat> gen = &genQuat;
    std::function<bool(const Quat&)> pred = [](const Quat& q) { return isRotation(quatWithoutNormalising(q)); };
    return witness::resolve<Quat>("fromQuaternion orthonormal, normalise ablated", gen, pred, Seed);
}

// -------- euler-round-trip: YXZ angles come back out of the matrix. --------
witness::Trial holdEulerRoundTrip()
{
    witness::Generator<Euler> gen = &genEuler;
    std::function<bool(const Euler&)> pred = [](const Euler& e) {
        const Rotation r = euler(e);
        float yaw = 0.0f;
        float pitch = 0.0f;
        float roll = 0.0f;
        toYawPitchRoll(r, yaw, pitch, roll);
        return worstEntry(r, fromEuler(EulerOrder::YXZ, yaw, pitch, roll)) < 1e-4;
    };
    return witness::resolve<Euler>("euler YXZ round-trips through yaw/pitch/roll", gen, pred, Seed);
}
witness::Trial ablateEulerRoundTrip()
{
    witness::Generator<Euler> gen = &genEuler;
    std::function<bool(const Euler&)> pred = [](const Euler& e) {
        return worstEntry(euler(e), fromEuler(EulerOrder::XYZ, float(e.a), float(e.b), float(e.c))) < 1e-4;
    };
    return witness::resolve<Euler>("euler round-trip, order ablated to XYZ", gen, pred, Seed);
}

// -------- quaternion-round-trip: matrix to quaternion to matrix is exact. --------
witness::Trial holdQuatRoundTrip()
{
    witness::Generator<Euler> gen = &genEuler;
    std::function<bool(const Euler&)> pred = [](const Euler& e) {
        const Rotation r = euler(e);
        float q[4];
        toQuaternion(r, q);
        Rotation back;
        if (!fromQuaternion(q, back))
            return false;
        return worstEntry(r, back) < 1e-4;
    };
    return witness::resolve<Euler>("matrix to quaternion to matrix is exact", gen, pred, Seed);
}
witness::Trial ablateQuatRoundTrip()
{
    witness::Generator<Quat> gen = &genQuat;
    std::function<bool(const Quat&)> pred = [](const Quat& q) {
        const Rotation r = quatWithoutNormalising(q);
        float back[4];
        toQuaternion(r, back);
        Rotation again;
        if (!fromQuaternion(back, again))
            return false;
        return worstEntry(r, again) < 1e-4;
    };
    return witness::resolve<Quat>("quaternion round-trip, normalise ablated", gen, pred, Seed);
}

// -------- multiply-closed: a product of rotations is a rotation. --------
witness::Trial holdMultiplyClosed()
{
    witness::Generator<TwoEuler> gen = &genTwoEuler;
    std::function<bool(const TwoEuler&)> pred = [](const TwoEuler& t) {
        return isRotation(multiply(euler(t.first), euler(t.second)));
    };
    return witness::resolve<TwoEuler>("a product of rotations is a rotation", gen, pred, Seed);
}
witness::Trial ablateMultiplyClosed()
{
    witness::Generator<TwoEuler> gen = &genTwoEuler;
    std::function<bool(const TwoEuler&)> pred = [](const TwoEuler& t) {
        return isRotation(sumInsteadOfProduct(euler(t.first), euler(t.second)));
    };
    return witness::resolve<TwoEuler>("multiply closed, product ablated to sum", gen, pred, Seed);
}

// -------- reach-lands: a reachable target gets the hand on it. --------
witness::Trial holdReachLands()
{
    witness::Generator<Reach> gen = genReach(0.1, armReach() * 0.9);
    std::function<bool(const Reach&)> pred = [](const Reach& rc) {
        AgentState s;
        BodyModel body;
        float shoulder[3];
        shoulderPosition(s, 1, body, shoulder);
        float target[3];
        targetFromShoulder(shoulder, rc, target);
        const ArmSolve arm = solveArm(s, 1, target, body);
        witness::assume(arm.reachable);
        double worst = 0.0;
        for (int k = 0; k < 3; ++k)
            worst = std::max(worst, double(std::abs(arm.hand.position[k] - target[k])));
        return worst < 3e-3; // 3 mm, about two stacked pennies
    };
    return witness::resolve<Reach>("a reachable target lands the hand", gen, pred, Seed);
}
witness::Trial ablateReachLands()
{
    witness::Generator<Reach> gen = genReach(0.1, armReach() * 0.9);
    std::function<bool(const Reach&)> pred = [](const Reach& rc) {
        AgentState s;
        BodyModel body;
        float shoulder[3];
        shoulderPosition(s, 1, body, shoulder);
        float target[3];
        targetFromShoulder(shoulder, rc, target);
        // The ablated solver ignores the target and leaves the hand at the shoulder.
        return distance3(shoulder, target) < 3e-3;
    };
    return witness::resolve<Reach>("reach lands, aim ablated to the shoulder", gen, pred, Seed);
}

// -------- reach-clamped: a far target stops the hand within the arm. --------
witness::Trial holdReachClamped()
{
    const double reach = armReach();
    witness::Generator<Reach> gen = genReach(reach * 1.5, reach * 4.0);
    std::function<bool(const Reach&)> pred = [reach](const Reach& rc) {
        AgentState s;
        BodyModel body;
        float shoulder[3];
        shoulderPosition(s, 1, body, shoulder);
        float target[3];
        targetFromShoulder(shoulder, rc, target);
        const ArmSolve arm = solveArm(s, 1, target, body);
        witness::assume(!arm.reachable);
        return distance3(arm.hand.position, shoulder) <= reach + 3e-3;
    };
    return witness::resolve<Reach>("a far target is clamped to the arm's reach", gen, pred, Seed);
}
witness::Trial ablateReachClamped()
{
    const double reach = armReach();
    witness::Generator<Reach> gen = genReach(reach * 1.5, reach * 4.0);
    std::function<bool(const Reach&)> pred = [reach](const Reach& rc) {
        AgentState s;
        BodyModel body;
        float shoulder[3];
        shoulderPosition(s, 1, body, shoulder);
        float target[3];
        targetFromShoulder(shoulder, rc, target);
        // The ablated solver drops the clamp and puts the hand on the far target.
        return distance3(target, shoulder) <= reach + 3e-3;
    };
    return witness::resolve<Reach>("reach clamp, length limit ablated", gen, pred, Seed);
}

struct Node
{
    const char* name;
    std::vector<std::string> deps;
    witness::Trial (*hold)();
    witness::Trial (*ablate)();
};

std::vector<Node> dag()
{
    return {
        {"euler-orthonormal", {}, holdEulerOrtho, ablateEulerOrtho},
        {"quaternion-orthonormal", {}, holdQuatOrtho, ablateQuatOrtho},
        {"euler-round-trip", {"euler-orthonormal"}, holdEulerRoundTrip, ablateEulerRoundTrip},
        {"quaternion-round-trip", {"quaternion-orthonormal"}, holdQuatRoundTrip, ablateQuatRoundTrip},
        {"multiply-closed", {"euler-orthonormal"}, holdMultiplyClosed, ablateMultiplyClosed},
        {"reach-lands", {"euler-orthonormal"}, holdReachLands, ablateReachLands},
        {"reach-clamped", {}, holdReachClamped, ablateReachClamped},
    };
}

// Kahn's algorithm: a DAG empties its queue, a cycle leaves nodes stranded.
bool acyclic(const std::vector<Node>& nodes)
{
    std::map<std::string, int> indegree;
    for (const Node& n : nodes)
        indegree[n.name] += 0;
    for (const Node& n : nodes)
        indegree[n.name] = int(n.deps.size());
    size_t removed = 0;
    std::vector<std::string> ready;
    for (const Node& n : nodes)
        if (indegree[n.name] == 0)
            ready.push_back(n.name);
    while (!ready.empty())
    {
        const std::string done = ready.back();
        ready.pop_back();
        ++removed;
        for (const Node& n : nodes)
            for (const std::string& d : n.deps)
                if (d == done && --indegree[n.name] == 0)
                    ready.push_back(n.name);
    }
    return removed == nodes.size();
}

void checkNode(const char* name)
{
    for (const Node& n : dag())
        if (std::string(n.name) == name)
        {
            const witness::Trial held = n.hold();
            check(noWitness(held), std::string(name) + " property holds; " + held.message);
            const witness::Trial caught = n.ablate();
            check(isWitness(caught), std::string(name) + " ablation is caught; " + caught.message);
            return;
        }
    check(false, std::string("unknown node ") + name);
}

void coverage()
{
    const std::vector<Node> nodes = dag();
    for (const Node& n : nodes)
        for (const std::string& d : n.deps)
        {
            bool exists = false;
            for (const Node& m : nodes)
                exists = exists || d == m.name;
            check(exists, std::string("edge ") + n.name + " -> " + d + " names a node");
        }
    check(acyclic(nodes), "the invariant dependency graph is acyclic");
    // Control: the acyclicity check must reject a cycle, or passing it certifies nothing.
    const std::vector<Node> cyclic = {{"a", {"b"}, holdEulerOrtho, ablateEulerOrtho},
                                      {"b", {"a"}, holdEulerOrtho, ablateEulerOrtho}};
    check(!acyclic(cyclic), "control: a two-node cycle is rejected");
    size_t held = 0;
    size_t caught = 0;
    for (const Node& n : nodes)
    {
        if (noWitness(n.hold()))
            ++held;
        if (isWitness(n.ablate()))
            ++caught;
    }
    check(held == nodes.size(), "every node's property holds across the ladder");
    check(caught == nodes.size(), "every node's ablation surfaces a witness");
    // Control: a true property put where an ablation belongs yields no witness, so a node whose
    // control is missing cannot be mistaken for one that has it.
    check(noWitness(holdEulerOrtho()), "control: a non-ablation surfaces no witness");
}

// Concrete witnesses the properties above generalise: the base of the dag, checked without the ladder.
void unitWitnesses()
{
    const float forward[3] = {0.0f, 0.0f, -1.0f};
    float turned[3];
    apply(fromEuler(EulerOrder::YXZ, 90.0f, 0.0f, 0.0f), forward, turned);
    check(std::abs(turned[0] + 1.0f) < 1e-5f && std::abs(turned[2]) < 1e-5f, "yaw 90 turns forward to -X");
    check(isRotation(Rotation()) && !isRotation(sheared(Rotation())), "the identity is a rotation, a shear is not");
    const float aboutY[4] = {0.0f, float(std::sin(Pi / 4.0)), 0.0f, float(std::cos(Pi / 4.0))};
    Rotation fromQ;
    check(fromQuaternion(aboutY, fromQ), "a unit quaternion converts");
    check(worstEntry(fromQ, fromEuler(EulerOrder::YXZ, 90.0f, 0.0f, 0.0f)) < 1e-5,
          "a quaternion about +Y matches the YXZ yaw of the same angle");
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Rotation unused;
    check(!fromQuaternion(zero, unused), "control: a zero quaternion is refused");
}

const std::map<std::string, std::function<void()>> cases = {
    {"witness.unit-concrete-witnesses", [] { unitWitnesses(); }},
    {"witness.euler-orthonormal", [] { checkNode("euler-orthonormal"); }},
    {"witness.quaternion-orthonormal", [] { checkNode("quaternion-orthonormal"); }},
    {"witness.euler-round-trip", [] { checkNode("euler-round-trip"); }},
    {"witness.quaternion-round-trip", [] { checkNode("quaternion-round-trip"); }},
    {"witness.multiply-closed", [] { checkNode("multiply-closed"); }},
    {"witness.reach-lands", [] { checkNode("reach-lands"); }},
    {"witness.reach-clamped", [] { checkNode("reach-clamped"); }},
    {"witnessdag.ablation-coverage", [] { coverage(); }},
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || cases.count(argv[1]) == 0)
    {
        std::fprintf(stderr, "usage: xrpilot-witness-tests <case>\n");
        return 2;
    }
    cases.at(argv[1])();
    return failures == 0 ? 0 : 1;
}
