#pragma once

// core's SwitchGroup driven by the desktop input layer instead of the TCA8418 key matrix.
//
// Event order and hold timing are those of the IMX Keys driver:
// - onPush runs before isButtonPressed() turns true, onRelease before it turns false;
// - while a button is down, update() calls onHold after holdTime ms and then repeats it
//   faster and faster (holdTime minus 0, 100, 190, 235, 255, 270 ms, the last one sticks).
//
// press()/release() take interface button indices (mtInterfaceButtonsDef). Several inputs
// can be bound to one button, so presses are counted and only the first press and the last
// release reach core.

#include "Platform.h"

#include <array>
#include <cstdint>

class DesktopButtons : public SwitchGroup
{
  public:
    static constexpr int kButtonCount = 48; // Keys::BUTTON_MAX
    static constexpr int kDefaultHoldTimeMs = 333;

    explicit DesktopButtons(Timer* timer) : timer(timer) {}

    void setHoldTime(int timeMs) override { holdTime = timeMs; }
    bool isButtonPressed(int buttonIdx) override;
    void setActionCallback(ActionType action, void (*callback)(uint8_t)) override;

    void press(int button);
    void release(int button);
    void releaseAll();
    // Call from the main loop, before core's update, to deliver hold repeats.
    void update();

  private:
    static constexpr int kHoldSteps = 6;
    static constexpr uint16_t kHoldTimeStepDiff[kHoldSteps] = { 0, 100, 190, 235, 255, 270 };

    Timer* timer;
    int holdTime = kDefaultHoldTimeMs;
    void (*onPush)(uint8_t) = nullptr;
    void (*onRelease)(uint8_t) = nullptr;
    void (*onHold)(uint8_t) = nullptr;

    std::array<uint8_t, kButtonCount> pressCount{};
    std::array<uint8_t, kButtonCount> buttonPush{};
    std::array<uint32_t, kButtonCount> holdTimeMs{};
    std::array<uint8_t, kButtonCount> holdFactor{};
};
