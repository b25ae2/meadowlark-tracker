#include "PlatformDesktop.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

// core's button handlers (mtActions.cpp). On IMX initHardware registers them.
void onButtonPush(uint8_t n);
void onButtonRelease(uint8_t n);
void onButtonHold(uint8_t n);

//------------------------------------------------------------------------------
// DesktopTimer

DesktopTimer::DesktopTimer() : start(std::chrono::steady_clock::now()) {}

uint32_t DesktopTimer::millis()
{
    return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
        .count());
}

uint32_t DesktopTimer::micros()
{
    return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
        .count());
}

void DesktopTimer::delay(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint32_t DesktopTimer::clockCycles()
{
    return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
        .count());
}

namespace
{
thread_local bool inAudioBlock = false;
thread_local uint32_t audioBlockStartUs = 0;
thread_local uint32_t scheduledMidiDelayUs = 0;
} // namespace

void DesktopTimer::markAudioBlockStart()
{
    inAudioBlock = true;
    audioBlockStartUs = micros();
}

void DesktopTimer::scheduleMidiClockUs(uint32_t delayUs, void (*callback)())
{
    if (!callback)
        return;
    // core asks for the delay from now; the scheduler wants it from the block's start.
    scheduledMidiDelayUs = inAudioBlock ? micros() - audioBlockStartUs + delayUs : 0;
    callback();
    scheduledMidiDelayUs = 0;
}

uint32_t DesktopTimer::midiDelayUs()
{
    return scheduledMidiDelayUs;
}

//------------------------------------------------------------------------------
// DesktopCodec

void DesktopCodec::setMasterVolume(float volume)
{
    // Sgtl5000Impl: 0 mutes; 0.01 .. 1.0 maps linearly onto CHIP_ANA_HP_CTRL 0x7F .. 0x00,
    // where each step is 0.5 dB and 0x00 is +12 dB.
    if (volume <= 0.0f)
    {
        volumeGain = 0.0f;
        return;
    }
    const float clamped = volume < 0.01f ? 0.01f : (volume > 1.0f ? 1.0f : volume);
    const auto level = static_cast<uint8_t>(127.0f + (clamped - 0.01f) * (0.0f - 127.0f) / (1.0f - 0.01f));
    float decibels = 12.0f - 0.5f * level;
    if (decibels > 0.0f)
        decibels = 0.0f;
    volumeGain = std::pow(10.0f, decibels / 20.0f);
}

void DesktopCodec::setInputType(InputType input)
{
    inputType = input;
    updateInputGain();
    noteInputUse();
}

void DesktopCodec::setLineInLevel(int level)
{
    // control_sgtl5000.cpp lineInLevel: CHIP_ANA_ADC_CTRL steps, clamped to 15.
    lineInStage = level < 0 ? 0 : (level > 15 ? 15 : level);
    updateInputGain();
    noteInputUse();
}

void DesktopCodec::setMicGain(int gainDb)
{
    // control_sgtl5000.cpp micGain: the preamp takes 40, 30 or 20 dB, the ADC stage the
    // rest in 1.5 dB steps (at most 15).
    int rest = gainDb < 0 ? 0 : gainDb;
    if (rest >= 40)
        rest -= 40;
    else if (rest >= 30)
        rest -= 30;
    else if (rest >= 20)
        rest -= 20;
    const int stage = rest * 2 / 3;
    micStage = stage > 15 ? 15 : stage;
    updateInputGain();
    noteInputUse();
}

void DesktopCodec::updateInputGain()
{
    const int stage = inputType == InputType::kMic ? micStage : lineInStage;
    inputLinearGain = std::pow(10.0f, stage * 1.5f / 20.0f);
}

void DesktopCodec::noteInputUse()
{
    if (inputArmed)
        inputRequested = true;
}

//------------------------------------------------------------------------------
// StubSwitchGroup

void StubSwitchGroup::setActionCallback(ActionType action, void (*callback)(uint8_t))
{
    if (action >= kPress && action <= kHold)
        callbacks[action] = callback;
}

//------------------------------------------------------------------------------
// FileConfigManager

namespace
{
constexpr char kConfigFilePath[] = "/trackerConfig";
}

bool FileConfigManager::readData(int address, void* data, int size)
{
    if (!fileSystem->exists(kConfigFilePath))
        return false;

    auto [isOpen, file] = fileSystem->openFile(kConfigFilePath);
    if (!isOpen)
        return false;

    fileSystem->seekFile(address, file);
    fileSystem->readFile(data, size, file);
    fileSystem->closeFile(file);
    return true;
}

void FileConfigManager::writeData(int address, void* data, int size)
{
    auto [isOpen, file] = fileSystem->openFile(kConfigFilePath, FileSystem::FileOpenMode::kWriteOpen);
    if (!isOpen)
        return;

    fileSystem->seekFile(address, file);
    fileSystem->writeFile(data, size, file);
    fileSystem->closeFile(file);
}

//------------------------------------------------------------------------------
// PlatformDesktop

PlatformDesktop::PlatformDesktop() : buttons(&timer), configManager(&fileSystem)
{
    static const char kBuildId[] = "desktop";
    systemInfo.buildId = kBuildId;

    buttons.setActionCallback(SwitchGroup::kPress, onButtonPush);
    buttons.setActionCallback(SwitchGroup::kRelease, onButtonRelease);
    buttons.setActionCallback(SwitchGroup::kHold, onButtonHold);
}

void PlatformDesktop::setPowerMode(bool mode)
{
    // false = power off on hardware; on desktop that means quit.
    if (!mode)
        powerOff = true;
}

void PlatformDesktop::resetMCU()
{
    std::fprintf(stderr, "meadowlark: resetMCU() requested (ignored on desktop)\n");
}

void PlatformDesktop::runBootloader(char* fileName)
{
    std::fprintf(stderr,
                 "meadowlark: firmware update requested (%s), not supported on desktop\n",
                 fileName ? fileName : "?");
}

void PlatformDesktop::setPowerButtonCallback(PowerButtonCallback callback)
{
    powerButtonCallback = callback;
}

void PlatformDesktop::setPowerButton(bool pressed)
{
    if (pressed == powerButtonPressed)
        return;
    powerButtonPressed = pressed;
    if (powerButtonCallback)
        powerButtonCallback(pressed ? 1 : 0);
}
