#include "DesktopButtons.h"

bool DesktopButtons::isButtonPressed(int buttonIdx)
{
    if (buttonIdx < 0 || buttonIdx >= kButtonCount)
        return false;
    return buttonPush[buttonIdx] != 0;
}

void DesktopButtons::setActionCallback(ActionType action, void (*callback)(uint8_t))
{
    switch (action)
    {
        case kPress:
            onPush = callback;
            break;
        case kRelease:
            onRelease = callback;
            break;
        case kHold:
            onHold = callback;
            break;
    }
}

void DesktopButtons::press(int button)
{
    if (button < 0 || button >= kButtonCount)
        return;
    if (pressCount[button]++ != 0)
        return;

    holdTimeMs[button] = timer->millis();
    holdFactor[button] = 0;
    if (onPush)
        onPush(static_cast<uint8_t>(button));
    buttonPush[button] = 1;
}

void DesktopButtons::release(int button)
{
    if (button < 0 || button >= kButtonCount || pressCount[button] == 0)
        return;
    if (--pressCount[button] != 0)
        return;

    if (onRelease)
        onRelease(static_cast<uint8_t>(button));
    buttonPush[button] = 0;
}

void DesktopButtons::releaseAll()
{
    for (int button = 0; button < kButtonCount; button++)
    {
        if (pressCount[button] == 0)
            continue;
        pressCount[button] = 1;
        release(button);
    }
}

void DesktopButtons::update()
{
    const uint32_t now = timer->millis();
    for (int i = 0; i < kButtonCount; i++)
    {
        if (buttonPush[i] && (now - holdTimeMs[i]) > static_cast<uint32_t>(holdTime - kHoldTimeStepDiff[holdFactor[i]]))
        {
            if (holdFactor[i] < kHoldSteps - 1)
                holdFactor[i]++;

            holdTimeMs[i] = now;
            if (onHold)
                onHold(static_cast<uint8_t>(i));
        }
    }
}
