// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Commands.h"
#include "xrpilot/Body.h"
#include "xrpilot/HumanInput.h"
#include "xrpilot/SpanLog.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

namespace xrpilot
{

namespace
{

std::string escape(const std::string& text)
{
    std::string out;
    for (char c : text)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20)
            out += c;
    }
    return out;
}

bool readFloats(std::istringstream& in, float* out, int count)
{
    for (int i = 0; i < count; ++i)
    {
        if (!(in >> out[i]) || !std::isfinite(out[i]))
            return false;
    }
    std::string extra;
    return !(in >> extra);
}

std::string number(float value)
{
    std::ostringstream out;
    out.precision(5);
    out << (std::isfinite(value) ? value : 0.0f);
    return out.str();
}

std::string poseJson(const Pose& pose)
{
    const float* m = pose.rotation.m;
    std::string rows;
    for (int row = 0; row < 3; ++row)
    {
        rows += std::string(row == 0 ? "[" : ",[") + number(m[row * 3]) + "," + number(m[row * 3 + 1]) + "," +
                number(m[row * 3 + 2]) + "]";
    }
    return "{\"position\":[" + number(pose.position[0]) + "," + number(pose.position[1]) + "," + number(pose.position[2]) +
           "],\"rotation\":[" + rows + "]}";
}

// Position then a rotation matrix row by row; false unless the matrix is a proper rotation.
bool readPose(std::istringstream& in, Pose& pose)
{
    float v[12];
    if (!readFloats(in, v, 12))
        return false;
    Rotation r;
    std::copy(v + 3, v + 12, r.m);
    if (!isRotation(r))
        return false;
    std::copy(v, v + 3, pose.position);
    pose.rotation = r;
    return true;
}

int handIndex(const std::string& name)
{
    return name == "left" ? 0 : name == "right" ? 1 : -1;
}


const char* ok = "{\"ok\":true}";

} // namespace

std::string jsonString(const std::string& text)
{
    return "\"" + escape(text) + "\"";
}

std::string errorJson(const std::string& message)
{
    return "{\"ok\":false,\"error\":\"" + escape(message) + "\"}";
}

std::string stateJson(const AgentState& state, const ClientStatus& status, uint64_t framesDecoded)
{
    float halfH = 0.0f;
    float halfV = 0.0f;
    eyeHalfFov(state, halfH, halfV);
    halfH *= 57.29578f;
    halfV *= 57.29578f;
    std::string hands;
    for (int i = 0; i < 2; ++i)
    {
        const HandState& h = state.hands[i];
        hands += std::string(i == 0 ? "\"left\":" : ",\"right\":") + "{\"present\":" + (h.present ? "true" : "false") +
                 ",\"pose\":" + poseJson(handPose(state, i)) + ",\"trigger\":" + number(h.trigger) + ",\"grip\":" + number(h.grip) +
                 ",\"stick\":[" + number(h.stick[0]) + "," + number(h.stick[1]) + "]}";
    }
    return "{\"ok\":true,\"connected\":" + std::string(status.connected ? "true" : "false") + ",\"server\":\"" +
           escape(status.server) + "\",\"server_name\":\"" + escape(status.serverName) +
           "\",\"eye\":{\"width\":" + std::to_string(status.renderWidth) + ",\"height\":" +
           std::to_string(status.renderHeight) + ",\"half_fov_horizontal\":" + number(halfH) +
           ",\"half_fov_vertical\":" + number(halfV) + ",\"ipd\":" + number(state.ipd) + "},\"head\":" +
           poseJson(state.head) + ",\"hands\":{" + hands + "},\"buttons\":" + std::to_string(state.buttons) +
           ",\"frames\":{\"assembled\":" + std::to_string(status.framesAssembled) + ",\"decoded\":" +
           std::to_string(framesDecoded) + ",\"dropped\":" + std::to_string(status.framesDropped) +
           "},\"tracking_sent\":" + std::to_string(status.trackingSent) + "}";
}

CommandResult runCommand(const std::string& line, AgentState& state, const ClientStatus& status, uint64_t framesDecoded)
{
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    CommandResult result;
    if (verb == "state")
    {
        result.reply = stateJson(state, status, framesDecoded);
    }
    else if (verb == "head")
    {
        Pose head;
        if (!readPose(in, head))
            return {errorJson("head needs x y z and a 3x3 rotation matrix row by row"), {}};
        state.head = head;
        result.reply = ok;
    }
    else if (verb == "hand")
    {
        std::string name;
        in >> name;
        const int i = handIndex(name);
        Pose pose;
        if (i < 0 || !readPose(in, pose))
            return {errorJson("hand needs left|right, x y z and a 3x3 rotation matrix row by row"), {}};
        state.hands[i].pose = pose;
        state.hands[i].present = true;
        state.hands[i].manual = true;
        result.reply = ok;
    }
    else if (verb == "reach")
    {
        std::string name;
        in >> name;
        const int i = handIndex(name);
        float target[3];
        if (i < 0 || !readFloats(in, target, 3))
            return {errorJson("reach needs left|right x y z"), {}};
        const ArmSolve arm = solveArm(state, i, target);
        state.hands[i].pose = arm.hand;
        state.hands[i].present = true;
        state.hands[i].manual = true;
        result.reply = std::string("{\"ok\":true,\"reachable\":") + (arm.reachable ? "true" : "false") +
                       ",\"distance\":" + number(arm.distance) + ",\"hand\":" + poseJson(arm.hand) +
                       ",\"elbow\":[" + number(arm.elbow[0]) + "," + number(arm.elbow[1]) + "," +
                       number(arm.elbow[2]) + "]}";
    }
    else if (verb == "present")
    {
        std::string name;
        in >> name;
        const int i = handIndex(name);
        float v[1];
        if (i < 0 || !readFloats(in, v, 1))
            return {errorJson("present needs left|right 0|1"), {}};
        state.hands[i].present = v[0] != 0.0f;
        result.reply = ok;
    }
    else if (verb == "button")
    {
        std::string name;
        in >> name;
        const uint32_t flag = buttonFlag(name);
        float v[1];
        if (flag == 0 || !readFloats(in, v, 1))
            return {errorJson("button needs a known name and 0|1"), {}};
        state.buttons = v[0] != 0.0f ? (state.buttons | flag) : (state.buttons & ~flag);
        result.reply = ok;
    }
    else if (verb == "trigger" || verb == "grip")
    {
        std::string name;
        in >> name;
        const int i = handIndex(name);
        float v[1];
        if (i < 0 || !readFloats(in, v, 1))
            return {errorJson(verb + " needs left|right and a value 0..1"), {}};
        (verb == "trigger" ? state.hands[i].trigger : state.hands[i].grip) = std::clamp(v[0], 0.0f, 1.0f);
        result.reply = ok;
    }
    else if (verb == "stick")
    {
        std::string name;
        in >> name;
        const int i = handIndex(name);
        float v[2];
        if (i < 0 || !readFloats(in, v, 2))
            return {errorJson("stick needs left|right x y"), {}};
        state.hands[i].stick[0] = std::clamp(v[0], -1.0f, 1.0f);
        state.hands[i].stick[1] = std::clamp(v[1], -1.0f, 1.0f);
        result.reply = ok;
    }
    else if (verb == "fov")
    {
        float v[1];
        if (!readFloats(in, v, 1) || v[0] < 30.0f || v[0] > 170.0f)
            return {errorJson("fov needs vertical degrees between 30 and 170"), {}};
        state.verticalFovDegrees = v[0];
        result.reply = ok;
    }
    else if (verb == "release")
    {
        const float aspect = state.eyeAspect;
        const float fov = state.verticalFovDegrees;
        const Pose head = state.head;
        state = AgentState();
        state.eyeAspect = aspect;
        state.verticalFovDegrees = fov;
        state.head = head;
        result.reply = ok;
    }
    else if (verb == "span")
    {
        SpanLine span;
        if (!parseSpanLine(line, span))
            return {errorJson("span needs begin <id> <tool> or end <id> <ok|error>"), {}};
        result.reply = ok;
    }
    else if (verb == "screenshot")
    {
        std::string path;
        std::getline(in >> std::ws, path);
        if (path.empty())
            return {errorJson("screenshot needs a path"), {}};
        result.screenshotPath = path;
    }
    else
    {
        result.reply = errorJson("unknown command: " + verb);
    }
    return result;
}

} // namespace xrpilot
