#include "PlatformDesktop.h"

#include <cstdio>
#include <cstring>
#include <thread>

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

PlatformDesktop::PlatformDesktop() : configManager(&fileSystem)
{
    static const char kBuildId[] = "desktop";
    systemInfo.buildId = kBuildId;
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
