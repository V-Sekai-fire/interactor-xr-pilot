// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "ControllerLayout.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace
{

bool Has(const std::vector<ControllerInput>& inputs, const std::string& path, ControllerSource source)
{
    for (const ControllerInput& input : inputs)
    {
        if (input.path == path)
            return input.source == source;
    }
    return false;
}

std::filesystem::path ModelJson(const char* name)
{
    const std::string model = std::string(name).substr(std::string("{oxrsys}").size());
    return std::filesystem::path(OXRSYS_RENDERMODELS_DIR) / model / (model + ".json");
}

std::string Text(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// The first rotate_xyz angle about X of a component, or NaN when the component is missing.
double ComponentPitch(const std::string& json, const std::string& component)
{
    const size_t at = json.find("\"" + component + "\"");
    const size_t rotate = at == std::string::npos ? at : json.find("\"rotate_xyz\"", at);
    const size_t open = rotate == std::string::npos ? rotate : json.find('[', rotate);
    if (open == std::string::npos)
        return std::nan("");
    return std::strtod(json.c_str() + open + 1, nullptr);
}

} // namespace

TEST_CASE("Controller layouts default to the frame controller", "[ControllerLayout]")
{
    CHECK(std::string(LayoutForControllers("frame").controllerType) == "frame_controller");
    CHECK(std::string(LayoutForControllers("").name) == "frame");
    CHECK(std::string(LayoutForControllers("quest").name) == "frame");
    CHECK(std::string(LayoutForControllers("index").controllerType) == "knuckles");
    CHECK(std::string(LayoutForControllers("touch_plus").controllerType) == "oculus_touch");
}

TEST_CASE("Controller layouts give each hand its own buttons and no path twice", "[ControllerLayout]")
{
    for (const char* name : {"frame", "index", "touch_plus"})
    {
        INFO(name);
        const ControllerLayout& layout = LayoutForControllers(name);
        for (const std::vector<ControllerInput>* hand : {&layout.left, &layout.right})
        {
            std::set<std::string> paths;
            for (const ControllerInput& input : *hand)
                CHECK(paths.insert(input.path).second);
            CHECK(Has(*hand, "/input/trigger/value", ControllerSource::Trigger));
            CHECK(Has(*hand, "/input/grip/value", ControllerSource::Grip));
        }
    }
    const ControllerLayout& frame = LayoutForControllers("frame");
    CHECK(Has(frame.left, "/input/dpad_down/click", ControllerSource::Lower));
    CHECK(Has(frame.right, "/input/a/click", ControllerSource::Lower));
    CHECK(Has(frame.right, "/input/menu/click", ControllerSource::Menu));
    // Control: the frame controller's left hand has a d-pad, not A.
    CHECK_FALSE(Has(frame.left, "/input/a/click", ControllerSource::Lower));
    CHECK(Has(LayoutForControllers("index").left, "/input/finger/index", ControllerSource::Trigger));
    CHECK(Has(LayoutForControllers("touch_plus").left, "/input/system/click", ControllerSource::Menu));
}

TEST_CASE("Each hand's hidden render model carries its controller's pose components", "[ControllerLayout]")
{
    for (const char* name : {"frame", "index", "touch_plus"})
    {
        const ControllerLayout& layout = LayoutForControllers(name);
        for (const char* model : {layout.renderModelLeft, layout.renderModelRight})
        {
            INFO(model);
            const std::filesystem::path json = ModelJson(model);
            REQUIRE(std::filesystem::exists(json));
            CHECK(std::filesystem::exists(json.parent_path() / "oxrsys_hidden.obj"));
            const std::string text = Text(json);
            CHECK(text.find("\"body\"") != std::string::npos);
            CHECK(!std::isnan(ComponentPitch(text, "tip")));
        }
    }
    // SteamVR's dashboard pointer follows /pose/tip; XR Pilot sends the raw pose 40 degrees above its aim
    // (controllerFromAim), so the frame controller's tip must point 40 degrees down to land on the aim.
    const ControllerLayout& frame = LayoutForControllers("frame");
    CHECK(ComponentPitch(Text(ModelJson(frame.renderModelLeft)), "tip") == -40.0);
    CHECK(ComponentPitch(Text(ModelJson(frame.renderModelRight)), "tip") == -40.0);
    // Control: the shared model this replaces had an untilted tip, which aimed the pointer 40 degrees high.
    CHECK(ComponentPitch(R"({"tip": {"component_local": {"rotate_xyz": [0.0, 0.0, 0.0]}}})", "tip") != -40.0);
    CHECK(std::isnan(ComponentPitch(R"({"body": {}})", "tip")));
}

TEST_CASE("Controller sources read the hand's own buttons", "[ControllerLayout]")
{
    oxr::protocol::TrackingPacket packet = {};
    packet.buttonState = oxr::protocol::BUTTON_X | oxr::protocol::BUTTON_MENU;
    packet.leftTrigger = 0.6f;
    packet.rightThumbstick[1] = -0.5f;
    CHECK(ControllerSourceValue(ControllerSource::Lower, packet, true) == 1.0f);
    CHECK(ControllerSourceValue(ControllerSource::Lower, packet, false) == 0.0f);
    CHECK(ControllerSourceValue(ControllerSource::TriggerClick, packet, true) == 1.0f);
    CHECK(ControllerSourceValue(ControllerSource::TriggerClick, packet, false) == 0.0f);
    CHECK(ControllerSourceValue(ControllerSource::StickY, packet, false) == -0.5f);
    CHECK(ControllerSourceValue(ControllerSource::StickTouch, packet, false) == 1.0f);
    CHECK(ControllerSourceValue(ControllerSource::StickTouch, packet, true) == 0.0f);
    CHECK(ControllerSourceValue(ControllerSource::Menu, packet, false) == 1.0f);
}
