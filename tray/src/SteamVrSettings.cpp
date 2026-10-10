// SPDX-License-Identifier: MPL-2.0

#include "xrpilot/SteamVrSettings.h"

namespace xrpilot
{

namespace
{

bool truthy(const Json* value)
{
    return value != nullptr && value->kind == Json::Kind::Bool && value->boolean;
}

Json boolean(bool value)
{
    Json j;
    j.kind = Json::Kind::Bool;
    j.boolean = value;
    return j;
}

} // namespace

bool applySteamVrHeadset(Json& settings, bool ours, MultipleDriversBefore& before)
{
    if (settings.kind != Json::Kind::Object)
        return false;
    Json steamvr;
    steamvr.kind = Json::Kind::Object;
    if (const Json* existing = settings.find("steamvr"))
        steamvr = *existing;
    const Json* forcedDriver = steamvr.find("forcedDriver");
    const bool forced = forcedDriver != nullptr && forcedDriver->str() == "oxrsys";
    const bool multiple = truthy(steamvr.find("activateMultipleDrivers"));
    bool changed = false;
    if (ours)
    {
        if (!forced)
        {
            steamvr.set("forcedDriver", Json::string("oxrsys"));
            changed = true;
        }
        if (!before.has_value())
            before = multiple;
        if (!multiple)
        {
            steamvr.set("activateMultipleDrivers", boolean(true));
            changed = true;
        }
    }
    else
    {
        if (forced)
            changed = steamvr.erase("forcedDriver") || changed;
        // Only a swap turned multiple drivers on; one the desk set itself stays as it is.
        if (before.has_value())
        {
            if (!*before && multiple)
                changed = steamvr.erase("activateMultipleDrivers") || changed;
            before.reset();
        }
    }
    if (changed)
        settings.set("steamvr", steamvr);
    return changed;
}

} // namespace xrpilot
