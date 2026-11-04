// Headless runner: initialises core against the desktop platform, runs the main loop with
// audio blocks rendered inline (no audio device yet) and reports what the engine and UI
// produced. The display is rendered by the software FT812, so the last frame can be saved.
//
//   meadowlark-headless [seconds] [--screenshot out.png] [--sd-root dir | --no-sd]
//                       [--press button@seconds ...]
//
// seconds defaults to 3; the SD card root to DesktopFileSystem::defaultRoot(). --press
// presses a Mini button (input.toml action name, e.g. "down@2.5") for 100 ms at that time,
// through the same path as the window's input.

#include "AudioLock.h"
#include "PlatformDesktop.h"
#include "PngWriter.h"
#include "Tracker.h"
#include "input/Actions.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr int kAudioBlockSamples = 128;
constexpr double kSampleRate = 44100.0;

PlatformDesktop platform;

struct ScriptedPress
{
    input::ActionId button;
    double at;
    bool pressed = false;
    bool released = false;
};

void renderAudioBlock(Tracker* tracker, int16_t* left, int16_t* right)
{
    // Stand-in for the audio interrupt: the block renders with the main loop locked out.
    AudioLock::acquire();
    tracker->updateAudioBuffers(left, right, kAudioBlockSamples);
    AudioLock::release();
}
} // namespace

int main(int argc, char** argv)
{
    double seconds = 3.0;
    const char* screenshotPath = nullptr;
    std::string sdRoot = DesktopFileSystem::defaultRoot();
    std::vector<ScriptedPress> presses;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
        {
            screenshotPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--sd-root") == 0 && i + 1 < argc)
        {
            sdRoot = argv[++i];
        }
        else if (std::strcmp(argv[i], "--no-sd") == 0)
        {
            sdRoot.clear();
        }
        else if (std::strcmp(argv[i], "--press") == 0 && i + 1 < argc)
        {
            std::string spec = argv[++i];
            size_t at = spec.find('@');
            input::ActionId button = 0;
            if (at == std::string::npos || !input::parseAction(spec.substr(0, at), button) ||
                !input::isButtonAction(button))
            {
                std::fprintf(stderr, "meadowlark: --press wants <button>@<seconds>, got %s\n", spec.c_str());
                return 2;
            }
            presses.push_back({ button, std::atof(spec.c_str() + at + 1) });
        }
        else
        {
            seconds = std::atof(argv[i]);
        }
    }

    if (!platform.desktopFileSystem().setRoot(sdRoot))
    {
        std::fprintf(stderr, "meadowlark: cannot use %s as the SD card\n", sdRoot.c_str());
        return 1;
    }
    std::printf("meadowlark: SD card %s\n", sdRoot.empty() ? "(none)" : sdRoot.c_str());

    Tracker* tracker = Tracker::getInstance();

    std::printf("meadowlark: starting display\n");
    tracker->getStartDisplayFunc()(&platform);

    std::printf("meadowlark: initialising core\n");
    tracker->init(&platform);

    std::printf("meadowlark: running headless for %.1f s\n", seconds);

    int16_t left[kAudioBlockSamples];
    int16_t right[kAudioBlockSamples];
    uint64_t audioBlocks = 0;
    uint64_t loopIterations = 0;
    int peak = 0;

    using clock = std::chrono::steady_clock;
    const auto blockPeriod = std::chrono::duration<double>(kAudioBlockSamples / kSampleRate);
    const auto start = clock::now();
    auto nextBlock = start;

    while (clock::now() - start < std::chrono::duration<double>(seconds))
    {
        while (clock::now() >= nextBlock)
        {
            renderAudioBlock(tracker, left, right);
            for (int i = 0; i < kAudioBlockSamples; i++)
            {
                peak = std::max(peak, std::abs(static_cast<int>(left[i])));
                peak = std::max(peak, std::abs(static_cast<int>(right[i])));
            }
            audioBlocks++;
            nextBlock += std::chrono::duration_cast<clock::duration>(blockPeriod);
        }

        const double elapsed = std::chrono::duration<double>(clock::now() - start).count();
        for (ScriptedPress& press : presses)
        {
            if (!press.pressed && elapsed >= press.at)
            {
                press.pressed = true;
                platform.desktopButtons().press(press.button);
            }
            else if (press.pressed && !press.released && elapsed >= press.at + 0.1)
            {
                press.released = true;
                platform.desktopButtons().release(press.button);
            }
        }
        platform.desktopButtons().update();

        tracker->update();
        loopIterations++;

        if (platform.powerOffRequested())
        {
            std::printf("meadowlark: core requested power off\n");
            break;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    const DesktopDisplayDriver& display = platform.desktopDisplay();
    const eve::Chip::Stats& eveStats = display.eve().stats();
    std::printf("meadowlark: done\n");
    std::printf("  main loop iterations : %llu\n", static_cast<unsigned long long>(loopIterations));
    std::printf("  audio blocks rendered: %llu (peak %d)\n",
                static_cast<unsigned long long>(audioBlocks),
                peak);
    std::printf("  display lists        : %llu\n", static_cast<unsigned long long>(display.listsSubmitted));
    std::printf("  frames swapped       : %llu\n", static_cast<unsigned long long>(display.eve().frameCount()));
    std::printf("  coprocessor commands : %llu\n", static_cast<unsigned long long>(eveStats.commandWords));
    std::printf("  display-list words   : %llu\n", static_cast<unsigned long long>(eveStats.displayListWords));
    std::printf("  unknown commands     : %llu\n", static_cast<unsigned long long>(eveStats.unknownCommands));
    std::printf("  RAM_G bytes uploaded : %llu\n", static_cast<unsigned long long>(display.ramgBytesWritten));

    if (screenshotPath != nullptr)
    {
        if (!writePng(screenshotPath, display.eve().frame(), eve::kScreenWidth, eve::kScreenHeight))
        {
            std::fprintf(stderr, "meadowlark: could not write %s\n", screenshotPath);
            return 1;
        }
        std::printf("  screenshot           : %s\n", screenshotPath);
    }
    return 0;
}
