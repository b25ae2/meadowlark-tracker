#include "Actions.h"

#include "core/interfaceDefs.h"

#include <cctype>
#include <cstdlib>

namespace input
{
static_assert(kButtonActionCount == interfaceButtonsCount);

namespace
{
struct NamedAction
{
    const char* name;
    ActionId action;
};

// The Tracker Mini's buttons, named after their labels. Several share an index on purpose:
// "fx2" and "vol" are the same button.
constexpr NamedAction kNames[] = {
    { "play", interfaceButtonPlay },
    { "rec", interfaceButtonRec },
    { "shift", interfaceButtonShift },
    { "copy", interfaceButtonCopy },
    { "insert", interfaceButtonInsert },
    { "delete", interfaceButtonDelete },
    { "up", interfaceButtonUp },
    { "down", interfaceButtonDown },
    { "left", interfaceButtonLeft },
    { "right", interfaceButtonRight },
    { "note", interfaceButtonNote },
    { "instr", interfaceButtonInstr },
    { "fx1", interfaceButtonFx1 },
    { "fx2", interfaceButtonFx2 },
    { "vol", interfaceButtonVol },
    { "pattern", interfaceButtonPattern },
    { "menu", interfaceButtonMiniMenu },
    { "middle", interfaceButtonMiniMiddle },
    { "fn1", interfaceButtonMiniFn1 },
    { "fn2", interfaceButtonMiniFn2 },
    { "fn3", interfaceButtonMiniFn3 },
    { "fn4", interfaceButtonMiniFn4 },
    { "plus10", interfaceButtonMiniPlus10 },
    { "plus1", interfaceButtonMiniPlus1 },
    { "minus1", interfaceButtonMiniMinus1 },
    { "minus10", interfaceButtonMiniMinus10 },
    { "power", kActionPower },
    { "fullscreen", kActionFullscreen },
    { "screenshot", kActionScreenshot },
    { "quit", kActionQuit },
};

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}
} // namespace

bool parseAction(std::string_view name, ActionId& out)
{
    for (const NamedAction& entry : kNames)
    {
        if (equalsIgnoreCase(name, entry.name))
        {
            out = entry.action;
            return true;
        }
    }

    constexpr std::string_view kButtonPrefix = "button";
    if (name.size() > kButtonPrefix.size() && equalsIgnoreCase(name.substr(0, kButtonPrefix.size()), kButtonPrefix))
    {
        std::string digits(name.substr(kButtonPrefix.size()));
        char* end = nullptr;
        long index = std::strtol(digits.c_str(), &end, 10);
        if (*end == '\0' && std::isdigit(static_cast<unsigned char>(digits[0])) && index < kButtonActionCount)
        {
            out = static_cast<ActionId>(index);
            return true;
        }
    }
    return false;
}

std::string actionName(ActionId action)
{
    for (const NamedAction& entry : kNames)
    {
        if (entry.action == action)
            return entry.name;
    }
    return "button" + std::to_string(action);
}
} // namespace input
