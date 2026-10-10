// SPDX-License-Identifier: MPL-2.0
//
// The two tracks of steamvr.vrsettings, as lean/XrPilot/Ui.lean models them (Desk.swapToOxrsys and
// Desk.handBackHeadset): swapping to OXRSys forces its headset and turns on multiple drivers, so
// trackers and other drivers still load; handing back removes the forced headset and puts multiple
// drivers back as the desk had it.

#pragma once

#include "xrpilot/Json.h"

#include <optional>

namespace xrpilot
{

// What multiple drivers was before the swap turned it on; empty when no swap is in effect.
using MultipleDriversBefore = std::optional<bool>;

// Changes settings (the whole steamvr.vrsettings) for a swap to OXRSys (ours) or the hand-back.
// before is the remembered value, kept across starts by the caller. Returns whether settings changed.
bool applySteamVrHeadset(Json& settings, bool ours, MultipleDriversBefore& before);

} // namespace xrpilot
