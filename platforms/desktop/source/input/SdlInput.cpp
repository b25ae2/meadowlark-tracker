#include "SdlInput.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace input
{
namespace
{
// InputId = kind << 32 | code.
enum Kind : uint32_t
{
    kKey = 1,     // code: SDL_Scancode
    kJoyButton,   // code: button index
    kJoyAxis,     // code: axis << 1 | negative
    kJoyHat,      // code: hat << 2 | direction
    kPadButton,   // code: SDL_GamepadButton
    kPadAxis,     // code: SDL_GamepadAxis << 1 | negative
    kMouseButton, // code: SDL_BUTTON_*
    kMouseWheel,  // code: direction
    kMouseMotion, // code: direction
};

enum Direction : uint32_t
{
    kUp,
    kDown,
    kLeft,
    kRight
};

constexpr const char* kDirectionNames[] = { "up", "down", "left", "right" };
constexpr const char* kMouseButtonNames[] = { nullptr, "left", "middle", "right", "x1", "x2" };
constexpr uint32_t kMouseButtonCount = 6;

InputId makeInput(Kind kind, uint32_t code)
{
    return static_cast<InputId>(kind) << 32 | code;
}

Kind kindOf(InputId input)
{
    return static_cast<Kind>(input >> 32);
}

uint32_t codeOf(InputId input)
{
    return static_cast<uint32_t>(input);
}

// Key for per-device axis and hat state.
uint64_t stateKey(uint32_t device, Kind kind, uint32_t index)
{
    return static_cast<uint64_t>(device) << 32 | static_cast<uint64_t>(kind) << 16 | index;
}

std::string lower(std::string_view text)
{
    std::string result(text);
    for (char& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

// "<prefix><number><rest>" -> number and rest.
bool splitIndexed(std::string_view text, std::string_view prefix, uint32_t& index, std::string_view& rest)
{
    if (text.substr(0, prefix.size()) != prefix)
        return false;
    text.remove_prefix(prefix.size());
    size_t digits = 0;
    while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits])))
        digits++;
    if (digits == 0 || digits > 3)
        return false;
    index = static_cast<uint32_t>(std::atoi(std::string(text.substr(0, digits)).c_str()));
    rest = text.substr(digits);
    return true;
}

bool parseDirection(std::string_view text, uint32_t& direction)
{
    for (uint32_t i = 0; i < 4; i++)
    {
        if (text == kDirectionNames[i])
        {
            direction = i;
            return true;
        }
    }
    return false;
}

bool parseSign(std::string_view text, uint32_t& negative)
{
    if (text == "+")
        negative = 0;
    else if (text == "-")
        negative = 1;
    else
        return false;
    return true;
}

bool isTrigger(SDL_GamepadAxis axis)
{
    return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
}

bool parseJoystick(std::string_view name, InputId& out)
{
    uint32_t index = 0;
    std::string_view rest;
    uint32_t value = 0;
    if (splitIndexed(name, "button", index, rest) && rest.empty())
        out = makeInput(kJoyButton, index);
    else if (splitIndexed(name, "axis", index, rest) && parseSign(rest, value))
        out = makeInput(kJoyAxis, index << 1 | value);
    else if (splitIndexed(name, "hat", index, rest) && parseDirection(rest, value))
        out = makeInput(kJoyHat, index << 2 | value);
    else
        return false;
    return true;
}

bool parseGamepad(std::string_view name, InputId& out)
{
    std::string base(name);
    uint32_t negative = 0;
    bool signGiven = false;
    if (!base.empty() && (base.back() == '+' || base.back() == '-'))
    {
        negative = base.back() == '-' ? 1 : 0;
        signGiven = true;
        base.pop_back();
    }

    SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(base.c_str());
    if (axis != SDL_GAMEPAD_AXIS_INVALID)
    {
        // Triggers only travel one way.
        if (isTrigger(axis) ? negative != 0 : !signGiven)
            return false;
        out = makeInput(kPadAxis, static_cast<uint32_t>(axis) << 1 | negative);
        return true;
    }

    SDL_GamepadButton button = SDL_GetGamepadButtonFromString(base.c_str());
    if (button == SDL_GAMEPAD_BUTTON_INVALID || signGiven)
        return false;
    out = makeInput(kPadButton, static_cast<uint32_t>(button));
    return true;
}

bool parseMouse(std::string_view name, InputId& out)
{
    for (uint32_t i = 1; i < kMouseButtonCount; i++)
    {
        if (name == kMouseButtonNames[i])
        {
            out = makeInput(kMouseButton, i);
            return true;
        }
    }

    uint32_t direction = 0;
    if (name.substr(0, 5) == "wheel" && parseDirection(name.substr(5), direction))
        out = makeInput(kMouseWheel, direction);
    else if (name.substr(0, 4) == "move" && parseDirection(name.substr(4), direction))
        out = makeInput(kMouseMotion, direction);
    else
        return false;
    return true;
}
} // namespace

bool parseInput(std::string_view name, InputId& out)
{
    size_t colon = name.find(':');
    if (colon == std::string_view::npos)
        return false;
    const std::string device = lower(name.substr(0, colon));
    const std::string_view rest = name.substr(colon + 1);

    if (device == "key")
    {
        // SDL matches scancode names case-insensitively.
        SDL_Scancode scancode = SDL_GetScancodeFromName(std::string(rest).c_str());
        if (scancode == SDL_SCANCODE_UNKNOWN)
            return false;
        out = makeInput(kKey, static_cast<uint32_t>(scancode));
        return true;
    }
    if (device == "joy")
        return parseJoystick(lower(rest), out);
    if (device == "pad")
        return parseGamepad(lower(rest), out);
    if (device == "mouse")
        return parseMouse(lower(rest), out);
    return false;
}

std::string inputName(InputId input)
{
    const uint32_t code = codeOf(input);
    switch (kindOf(input))
    {
        case kKey:
            return std::string("key:") + SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
        case kJoyButton:
            return "joy:button" + std::to_string(code);
        case kJoyAxis:
            return "joy:axis" + std::to_string(code >> 1) + ((code & 1) ? "-" : "+");
        case kJoyHat:
            return "joy:hat" + std::to_string(code >> 2) + kDirectionNames[code & 3];
        case kPadButton:
        {
            const char* name = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(code));
            return std::string("pad:") + (name ? name : "?");
        }
        case kPadAxis:
        {
            auto axis = static_cast<SDL_GamepadAxis>(code >> 1);
            const char* name = SDL_GetGamepadStringForAxis(axis);
            std::string result = std::string("pad:") + (name ? name : "?");
            if (!isTrigger(axis))
                result += (code & 1) ? "-" : "+";
            return result;
        }
        case kMouseButton:
            return std::string("mouse:") + (code < kMouseButtonCount && kMouseButtonNames[code] ? kMouseButtonNames[code] : "?");
        case kMouseWheel:
            return std::string("mouse:wheel") + kDirectionNames[code & 3];
        case kMouseMotion:
            return std::string("mouse:move") + kDirectionNames[code & 3];
    }
    return "?";
}

//------------------------------------------------------------------------------
// SdlInput

SdlInput::~SdlInput()
{
    for (auto& [id, gamepad] : gamepads)
        SDL_CloseGamepad(gamepad);
    for (auto& [id, joystick] : joysticks)
        SDL_CloseJoystick(joystick);
}

bool SdlInput::handleEvent(const SDL_Event& event)
{
    switch (event.type)
    {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            if (event.key.repeat)
                return true;
            const InputId input = makeInput(kKey, static_cast<uint32_t>(event.key.scancode));
            event.key.down ? sourceDown(0, input) : sourceUp(0, input);
            return true;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            const InputId input = makeInput(kMouseButton, event.button.button);
            event.button.down ? sourceDown(0, input) : sourceUp(0, input);
            return true;
        }

        case SDL_EVENT_MOUSE_WHEEL:
        {
            const float sign = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            wheelX += event.wheel.x * sign;
            wheelY += event.wheel.y * sign;
            for (; wheelY >= 1.0f; wheelY -= 1.0f)
                tap(makeInput(kMouseWheel, kUp));
            for (; wheelY <= -1.0f; wheelY += 1.0f)
                tap(makeInput(kMouseWheel, kDown));
            for (; wheelX >= 1.0f; wheelX -= 1.0f)
                tap(makeInput(kMouseWheel, kRight));
            for (; wheelX <= -1.0f; wheelX += 1.0f)
                tap(makeInput(kMouseWheel, kLeft));
            return true;
        }

        case SDL_EVENT_MOUSE_MOTION:
        {
            const float step = options.motionStep > 1.0f ? options.motionStep : 1.0f;
            motionX += event.motion.xrel;
            motionY += event.motion.yrel;
            for (; motionY >= step; motionY -= step)
                tap(makeInput(kMouseMotion, kDown));
            for (; motionY <= -step; motionY += step)
                tap(makeInput(kMouseMotion, kUp));
            for (; motionX >= step; motionX -= step)
                tap(makeInput(kMouseMotion, kRight));
            for (; motionX <= -step; motionX += step)
                tap(makeInput(kMouseMotion, kLeft));
            return true;
        }

        case SDL_EVENT_JOYSTICK_ADDED:
            openDevice(event.jdevice.which);
            return true;

        case SDL_EVENT_JOYSTICK_REMOVED:
            closeDevice(event.jdevice.which);
            return true;

        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
        {
            const InputId input = makeInput(kJoyButton, event.jbutton.button);
            event.jbutton.down ? sourceDown(event.jbutton.which, input) : sourceUp(event.jbutton.which, input);
            return true;
        }

        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        {
            const uint32_t axis = event.jaxis.axis;
            axisMotion(event.jaxis.which,
                       makeInput(kJoyAxis, axis << 1),
                       makeInput(kJoyAxis, axis << 1 | 1),
                       stateKey(event.jaxis.which, kJoyAxis, axis),
                       event.jaxis.value / 32767.0f);
            return true;
        }

        case SDL_EVENT_JOYSTICK_HAT_MOTION:
            hatMotion(event.jhat.which, event.jhat.hat, event.jhat.value);
            return true;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        {
            const InputId input = makeInput(kPadButton, event.gbutton.button);
            event.gbutton.down ? sourceDown(event.gbutton.which, input) : sourceUp(event.gbutton.which, input);
            return true;
        }

        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        {
            const uint32_t axis = event.gaxis.axis;
            axisMotion(event.gaxis.which,
                       makeInput(kPadAxis, axis << 1),
                       makeInput(kPadAxis, axis << 1 | 1),
                       stateKey(event.gaxis.which, kPadAxis, axis),
                       event.gaxis.value / 32767.0f);
            return true;
        }

        default:
            return false;
    }
}

void SdlInput::releaseAll()
{
    std::vector<DeviceId> devices;
    for (auto& [device, inputs] : held)
        devices.push_back(device);
    for (DeviceId device : devices)
        releaseDevice(device);
    axisState.clear();
    hatState.clear();
    wheelX = wheelY = motionX = motionY = 0.0f;
}

void SdlInput::sourceDown(DeviceId device, InputId input)
{
    if (!held[device].insert(input).second)
        return;
    if (trace)
        std::printf("input: %s\n", inputName(input).c_str());
    if (holders[input]++ == 0)
        resolver.inputDown(input);
}

void SdlInput::sourceUp(DeviceId device, InputId input)
{
    auto it = held.find(device);
    if (it == held.end() || it->second.erase(input) == 0)
        return;
    if (--holders[input] == 0)
        resolver.inputUp(input);
}

void SdlInput::tap(InputId input)
{
    sourceDown(0, input);
    sourceUp(0, input);
}

void SdlInput::releaseDevice(DeviceId device)
{
    auto it = held.find(device);
    if (it == held.end())
        return;
    std::vector<InputId> inputs(it->second.begin(), it->second.end());
    for (InputId input : inputs)
        sourceUp(device, input);
    held.erase(device);
}

void SdlInput::openDevice(SDL_JoystickID id)
{
    if (joysticks.count(id) || gamepads.count(id))
        return;

    // A gamepad opened through SDL_OpenGamepad delivers both gamepad and joystick events.
    if (SDL_IsGamepad(id))
    {
        if (SDL_Gamepad* gamepad = SDL_OpenGamepad(id))
        {
            gamepads[id] = gamepad;
            std::printf("meadowlark: gamepad \"%s\" connected\n", SDL_GetGamepadName(gamepad));
            return;
        }
    }
    if (SDL_Joystick* joystick = SDL_OpenJoystick(id))
    {
        joysticks[id] = joystick;
        std::printf("meadowlark: joystick \"%s\" connected\n", SDL_GetJoystickName(joystick));
    }
    else
    {
        std::fprintf(stderr, "meadowlark: cannot open joystick %u: %s\n", static_cast<unsigned>(id), SDL_GetError());
    }
}

void SdlInput::closeDevice(SDL_JoystickID id)
{
    releaseDevice(id);
    for (auto it = axisState.begin(); it != axisState.end();)
        it = (it->first >> 32) == id ? axisState.erase(it) : std::next(it);
    for (auto it = hatState.begin(); it != hatState.end();)
        it = (it->first >> 32) == id ? hatState.erase(it) : std::next(it);

    if (auto it = gamepads.find(id); it != gamepads.end())
    {
        SDL_CloseGamepad(it->second);
        gamepads.erase(it);
    }
    if (auto it = joysticks.find(id); it != joysticks.end())
    {
        SDL_CloseJoystick(it->second);
        joysticks.erase(it);
    }
    std::printf("meadowlark: joystick %u disconnected\n", static_cast<unsigned>(id));
}

void SdlInput::axisMotion(DeviceId device, InputId positive, InputId negative, uint64_t key, float value)
{
    // Hysteresis: a direction presses past axisPress and releases below axisRelease, so an
    // axis resting near the threshold doesn't chatter.
    int& state = axisState[key];
    int next = state;
    if (state == 0 || std::fabs(value) < options.axisRelease || (value > 0) != (state > 0))
        next = 0;
    if (value >= options.axisPress)
        next = 1;
    else if (value <= -options.axisPress)
        next = -1;
    if (next == state)
        return;

    if (state > 0)
        sourceUp(device, positive);
    else if (state < 0)
        sourceUp(device, negative);
    state = next;
    if (state > 0)
        sourceDown(device, positive);
    else if (state < 0)
        sourceDown(device, negative);
}

void SdlInput::hatMotion(DeviceId device, int hat, uint8_t value)
{
    constexpr uint8_t kMasks[4] = { SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT };
    uint8_t& previous = hatState[stateKey(device, kJoyHat, static_cast<uint32_t>(hat))];
    for (uint32_t direction = 0; direction < 4; direction++)
    {
        const bool was = (previous & kMasks[direction]) != 0;
        const bool is = (value & kMasks[direction]) != 0;
        if (was == is)
            continue;
        const InputId input = makeInput(kJoyHat, static_cast<uint32_t>(hat) << 2 | direction);
        is ? sourceDown(device, input) : sourceUp(device, input);
    }
    previous = value;
}
} // namespace input
