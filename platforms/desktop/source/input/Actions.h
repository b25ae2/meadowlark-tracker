#pragma once

// What an input can be bound to: one of core's interface buttons (mtInterfaceButtonsDef,
// pressed through DesktopButtons) or a desktop command handled by the front end.

#include <string>
#include <string_view>

namespace input
{
using ActionId = int;

// 0 .. kButtonActionCount-1 are interface button indices.
constexpr ActionId kButtonActionCount = 44;

enum : ActionId
{
    kActionPower = 64, // the Mini's power button: hold to save and power off
    kActionFullscreen,
    kActionScreenshot,
    kActionQuit, // same as closing the window
    kActionEnd
};

inline bool isButtonAction(ActionId action)
{
    return action >= 0 && action < kButtonActionCount;
}

// Names as used in input.toml: function names ("play", "fn1"), "button<n>" for any interface
// button, and the commands. Case-insensitive.
bool parseAction(std::string_view name, ActionId& out);
// Function name where there is one, else "button<n>".
std::string actionName(ActionId action);
} // namespace input
