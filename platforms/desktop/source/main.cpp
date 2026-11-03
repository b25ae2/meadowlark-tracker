// Headless runner: initialises core against the desktop platform, runs the main loop with
// audio blocks rendered inline (no audio device yet) and reports what the engine and UI
// produced. The display is rendered by the software FT812, so the last frame can be saved.
//
//   meadowlark-headless [seconds] [--screenshot out.png]     seconds default: 3

#include "AudioLock.h"
#include "PlatformDesktop.h"
#include "PngWriter.h"
#include "Tracker.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
constexpr int kAudioBlockSamples = 128;
constexpr double kSampleRate = 44100.0;

PlatformDesktop platform;

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
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
        {
            screenshotPath = argv[++i];
        }
        else
        {
            seconds = std::atof(argv[i]);
        }
    }

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
