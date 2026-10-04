// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// The line protocol the MCP server speaks to the pilot over stdio, one command per line and one JSON
// object per reply line:
//   state
//   head <x> <y> <z> <r00> ... <r22>               metres, then the rotation matrix row by row
//   hand <left|right> <x> <y> <z> <r00> ... <r22>  places and shows the hand
//   reach <left|right> <x> <y> <z>                  the body's arm reaches for a world point
//   present <left|right> <0|1>
//   button <a|b|x|y|menu|left_thumbstick|right_thumbstick|headset> <0|1>
//   trigger|grip <left|right> <0..1>
//   stick <left|right> <x> <y>
//   fov <vertical degrees>
//   release                                        everything back to rest
//   span begin <id> <tool> [args] | span end <id> <ok|error>   trace marks for the command view
//   screenshot <path>                              left eye as PNG, handled by the caller on the GPU thread

#pragma once

#include "xrpilot/Agent.h"
#include "xrpilot/Client.h"

#include <cstdint>
#include <string>

namespace xrpilot
{

struct CommandResult
{
    std::string reply;          // empty when screenshotPath is set
    std::string screenshotPath; // set for a screenshot, which the caller performs
};

// Applies one command to state; status feeds the state reply.
CommandResult runCommand(const std::string& line, AgentState& state, const ClientStatus& status, uint64_t framesDecoded);

std::string stateJson(const AgentState& state, const ClientStatus& status, uint64_t framesDecoded);
std::string errorJson(const std::string& message);
// A JSON string literal, quotes included.
std::string jsonString(const std::string& text);

} // namespace xrpilot
