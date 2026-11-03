#pragma once

// SDL3 events to InputResolver inputs.
//
// Input names, as written in input.toml:
//   key:<scancode name>   physical key, SDL's names: "key:A", "key:Left Shift", "key:F1"
//   pad:<button>          gamepad button by SDL name: "pad:a", "pad:dpup", "pad:start"
//   pad:<axis>+ / -       gamepad axis direction: "pad:leftx-"; triggers may omit the sign
//   joy:button<n>         raw joystick button, "joy:axis<n>+" / "-", "joy:hat<n>up" (down/left/right)
//   mouse:<button>        left middle right x1 x2
//   mouse:wheelup         one tap per wheel step (wheeldown, wheelleft, wheelright)
//   mouse:moveup          one tap per motionStep pixels of pointer or trackball motion
//                         (movedown, moveleft, moveright)
//
// All joysticks and gamepads are merged: "pad:a" is the A button of any of them. An input
// held on two devices at once is released when both let go. Gamepads also report as raw
// joysticks, so both name forms work for them.

#include "InputResolver.h"

#include <SDL3/SDL.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace input
{
bool parseInput(std::string_view name, InputId& out);
std::string inputName(InputId input);

class SdlInput
{
  public:
    struct Options
    {
        float axisPress = 0.5f;   // axis travel that presses a direction
        float axisRelease = 0.35f; // and that releases it
        float motionStep = 40.0f; // pixels per mouse:move* tap
    };

    explicit SdlInput(InputResolver& resolver) : resolver(resolver) {}
    ~SdlInput();

    void setOptions(const Options& newOptions) { options = newOptions; }
    // Prints every input that goes down, bound or not.
    void setTrace(bool enabled) { trace = enabled; }

    // Returns true for the events it handles (keyboard, mouse, joystick, gamepad).
    bool handleEvent(const SDL_Event& event);
    // For focus loss: nothing stays held while the window can't see the releases.
    void releaseAll();

  private:
    using DeviceId = uint32_t; // SDL_JoystickID; 0 is the keyboard and mouse

    void sourceDown(DeviceId device, InputId input);
    void sourceUp(DeviceId device, InputId input);
    void tap(InputId input);
    void releaseDevice(DeviceId device);
    void openDevice(SDL_JoystickID id);
    void closeDevice(SDL_JoystickID id);
    void axisMotion(DeviceId device, InputId positive, InputId negative, uint64_t stateKey, float value);
    void hatMotion(DeviceId device, int hat, uint8_t value);

    InputResolver& resolver;
    Options options;
    bool trace = false;

    std::unordered_map<DeviceId, std::unordered_set<InputId>> held;
    std::unordered_map<InputId, int> holders;
    std::unordered_map<uint64_t, int> axisState; // -1, 0, +1 per (device, kind, axis)
    std::unordered_map<uint64_t, uint8_t> hatState;
    std::unordered_map<SDL_JoystickID, SDL_Joystick*> joysticks;
    std::unordered_map<SDL_JoystickID, SDL_Gamepad*> gamepads;

    float wheelX = 0.0f;
    float wheelY = 0.0f;
    float motionX = 0.0f;
    float motionY = 0.0f;
};
} // namespace input
