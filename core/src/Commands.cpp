// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Commands.h"

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

std::string number(float value)
{
    std::ostringstream out;
    out.precision(5);
    out << (std::isfinite(value) ? value : 0.0f);
    return out.str();
}

std::string poseJson(const Pose& pose)
{
    return "{\"position\":[" + number(pose.position[0]) + "," + number(pose.position[1]) + "," + number(pose.position[2]) +
           "],\"yaw\":" + number(pose.yaw) + ",\"pitch\":" + number(pose.pitch) + ",\"roll\":" + number(pose.roll) + "}";
}

int handIndex(const std::string& name)
{
    return name == "left" ? 0 : name == "right" ? 1 : -1;
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
    const float halfV = state.verticalFovDegrees * 0.5f;
    const float halfH = std::atan(std::tan(halfV * 0.017453292f) * state.eyeAspect) * 57.29578f;
    std::string hands;
    for (int i = 0; i < 2; ++i)
    {
        const HandState& h = state.hands[i];
        hands += std::string(i == 0 ? "\"left\":" : ",\"right\":") + "{\"present\":" + (h.present ? "true" : "false") +
                 ",\"pose\":" + poseJson(h.pose) + ",\"trigger\":" + number(h.trigger) + ",\"grip\":" + number(h.grip) +
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
        float v[6];
        if (!readFloats(in, v, 6))
            return {errorJson("head needs x y z yaw pitch roll"), {}};
        std::copy(v, v + 3, state.head.position);
        state.head.yaw = v[3];
        state.head.pitch = std::clamp(v[4], -89.0f, 89.0f);
        state.head.roll = v[5];
        result.reply = ok;
    }
    else if (verb == "hand")
    {
        std::string name;
        in >> name;
        const int i = handIndex(name);
        float v[5];
        if (i < 0 || !readFloats(in, v, 5))
            return {errorJson("hand needs left|right x y z yaw pitch"), {}};
        std::copy(v, v + 3, state.hands[i].pose.position);
        state.hands[i].pose.yaw = v[3];
        state.hands[i].pose.pitch = v[4];
        state.hands[i].pose.roll = 0.0f;
        state.hands[i].present = true;
        result.reply = ok;
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
