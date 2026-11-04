// Headless runner: initialises core against the desktop platform, runs the main loop with
// audio blocks rendered inline (no audio device yet) and reports what the engine and UI
// produced. The display is rendered by the software FT812, so the last frame can be saved.
//
//   meadowlark-headless [seconds] [--screenshot out.png] [--sd-root dir | --no-sd]
//                       [--press button@seconds[:hold] ...] [--no-host-folders] [--midi-trace]
//                       [--ptfont file] [--plugin clap:file[#id]@channel | lv2:uri@channel ...]
//                       [--note channel@seconds[:hold] ...]
//
// seconds defaults to 3; the SD card root to DesktopFileSystem::defaultRoot(). --press
// presses a Mini button (input.toml action name, e.g. "down@2.5") at that time for hold
// seconds (default 0.1), through the same path as the window's input. --midi-trace turns
// on MIDI clock, transport and note output on both ports (after 1 s, once core has read its
// config) and prints what core sends. --ptfont puts a font file in the card's /Fonts and
// switches the display to it at 1 s, as choosing it in Config would. --plugin loads an
// instrument plugin (built with the SDL front end), and --note sends core's MIDI-instrument
// note (middle C, full velocity) on a channel, as the sequencer would.

#include "AudioLock.h"
#include "CrashHandler.h"
#include "PlatformDesktop.h"
#include "PngWriter.h"
#include "ThemeManager.h"
#include "Tracker.h"
#include "input/Actions.h"
#include "mtMidi.h"
#include "mtStructs.h"
#ifdef MEADOWLARK_HAVE_PLUGINS
#include "plugins/PluginHost.h"
#endif

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
    double hold;
    bool pressed = false;
    bool released = false;
};

struct ScriptedNote
{
    int channel;
    double at;
    double hold;
    bool on = false;
    bool off = false;
};

// Prints outgoing MIDI with the audio block it was sent in and its offset into the block.
class TraceSink : public midi::Sink
{
  public:
    uint64_t block = 0;
    uint64_t clocks = 0;
    uint64_t firstClockBlock = 0;
    uint64_t lastClockBlock = 0;
    uint32_t lastClockDelayUs = 0;

    void send(int port, const midi::Message& message, uint32_t delayUs) override
    {
        if (message.status == 0xF8)
        {
            if (clocks++ == 0)
                firstClockBlock = block;
            lastClockBlock = block;
            lastClockDelayUs = delayUs;
            if (clocks > 4)
                return; // just the first few
        }
        std::printf("midi %-4s block %6llu +%4u us: %02X %02X %02X\n",
                    midi::portName(port),
                    static_cast<unsigned long long>(block),
                    delayUs,
                    message.status,
                    message.data1,
                    message.data2);
    }
};
TraceSink traceSink;

void renderAudioBlock(Tracker* tracker, int16_t* left, int16_t* right)
{
    // Stand-in for the audio interrupt: the block renders with the main loop locked out.
    platform.desktopTimer().markAudioBlockStart();
    AudioLock::acquire();
    tracker->updateAudioBuffers(left, right, kAudioBlockSamples);
    AudioLock::release();
    traceSink.block++;
}
} // namespace

int main(int argc, char** argv)
{
    installCrashHandler();
    double seconds = 3.0;
    const char* screenshotPath = nullptr;
    std::string sdRoot = DesktopFileSystem::defaultRoot();
    std::vector<ScriptedPress> presses;
    bool hostFolders = true;
    bool midiTrace = false;
    std::string ptFont;
    std::vector<ScriptedNote> notes;
#ifdef MEADOWLARK_HAVE_PLUGINS
    plugins::PluginHost pluginHost;
    std::vector<plugins::SlotConfig> pluginSlots;
#endif
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
        else if (std::strcmp(argv[i], "--no-host-folders") == 0)
        {
            hostFolders = false;
        }
        else if (std::strcmp(argv[i], "--midi-trace") == 0)
        {
            midiTrace = true;
        }
        else if (std::strcmp(argv[i], "--ptfont") == 0 && i + 1 < argc)
        {
            ptFont = argv[++i];
        }
        else if (std::strcmp(argv[i], "--note") == 0 && i + 1 < argc)
        {
            const std::string spec = argv[++i];
            const size_t at = spec.find('@');
            const int channel = std::atoi(spec.c_str());
            if (at == std::string::npos || channel < 1 || channel > 16)
            {
                std::fprintf(stderr, "meadowlark: --note wants <channel>@<seconds>[:hold], got %s\n", spec.c_str());
                return 2;
            }
            const size_t colon = spec.find(':', at);
            const double hold = colon == std::string::npos ? 0.5 : std::atof(spec.c_str() + colon + 1);
            notes.push_back({ channel, std::atof(spec.c_str() + at + 1), hold });
        }
#ifdef MEADOWLARK_HAVE_PLUGINS
        else if (std::strcmp(argv[i], "--plugin") == 0 && i + 1 < argc)
        {
            const std::string spec = argv[++i];
            const size_t colon = spec.find(':');
            const size_t at = spec.rfind('@');
            plugins::SlotConfig slot;
            slot.format = spec.substr(0, colon);
            if (colon == std::string::npos || at == std::string::npos || at < colon ||
                (slot.format != "clap" && slot.format != "lv2"))
            {
                std::fprintf(stderr, "meadowlark: --plugin wants clap:file[#id]@channel or lv2:uri@channel\n");
                return 2;
            }
            slot.location = spec.substr(colon + 1, at - colon - 1);
            if (slot.format == "clap" && slot.location.find('#') != std::string::npos)
            {
                slot.id = slot.location.substr(slot.location.find('#') + 1);
                slot.location.resize(slot.location.find('#'));
            }
            slot.channel = std::atoi(spec.c_str() + at + 1);
            pluginSlots.push_back(slot);
        }
#endif
        else if (std::strcmp(argv[i], "--press") == 0 && i + 1 < argc)
        {
            std::string spec = argv[++i];
            size_t at = spec.find('@');
            input::ActionId button = 0;
            if (at == std::string::npos || !input::parseAction(spec.substr(0, at), button) ||
                !input::isButtonAction(button))
            {
                std::fprintf(stderr, "meadowlark: --press wants <button>@<seconds>[:hold], got %s\n", spec.c_str());
                return 2;
            }
            const size_t colon = spec.find(':', at);
            const double hold = colon == std::string::npos ? 0.1 : std::atof(spec.c_str() + colon + 1);
            presses.push_back({ button, std::atof(spec.c_str() + at + 1), hold });
        }
        else
        {
            seconds = std::atof(argv[i]);
        }
    }

    if (hostFolders)
    {
        platform.desktopFileSystem().addDefaultHostMounts();
    }
    if (!platform.desktopFileSystem().setRoot(sdRoot))
    {
        std::fprintf(stderr, "meadowlark: cannot use %s as the SD card\n", sdRoot.c_str());
        return 1;
    }
    std::printf("meadowlark: SD card %s\n", sdRoot.empty() ? "(none)" : sdRoot.c_str());
    std::string ptFontName;
    if (!ptFont.empty())
    {
        ptFontName = ptFont.substr(ptFont.find_last_of('/') + 1);
        platform.desktopFileSystem().addVirtualFile("Fonts/" + ptFontName, ptFont);
        ptFontName = ptFontName.substr(0, ptFontName.rfind('.'));
    }
    bool fontApplied = ptFont.empty();

#ifdef MEADOWLARK_HAVE_PLUGINS
    for (const plugins::SlotConfig& slot : pluginSlots)
    {
        std::string error;
        if (!pluginHost.add(slot, error))
        {
            std::fprintf(stderr, "meadowlark: plugin: %s\n", error.c_str());
            return 1;
        }
    }
    if (pluginHost.size() > 0)
    {
        pluginHost.setCardRoot(platform.desktopFileSystem().root());
        platform.setExternalInstruments(&pluginHost);
    }
#endif

    Tracker* tracker = Tracker::getInstance();

    std::printf("meadowlark: starting display\n");
    tracker->getStartDisplayFunc()(&platform);

    std::printf("meadowlark: initialising core\n");
    tracker->init(&platform);
    if (midiTrace)
    {
        for (int port = 0; port < midi::kPortCount; port++)
            platform.midiPort(port).setSink(&traceSink);
    }
    // The interface reads the saved config in its start tasks, after the first updates.
    bool midiConfigured = !midiTrace;

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
            else if (press.pressed && !press.released && elapsed >= press.at + press.hold)
            {
                press.released = true;
                platform.desktopButtons().release(press.button);
            }
        }
        platform.desktopButtons().update();

        for (ScriptedNote& note : notes)
        {
            if (!note.on && elapsed >= note.at)
            {
                note.on = true;
                sendMidiNoteOn(60, 127, static_cast<uint8_t>(note.channel), false);
            }
            else if (note.on && !note.off && elapsed >= note.at + note.hold)
            {
                note.off = true;
                sendMidiNoteOff(60, 0, static_cast<uint8_t>(note.channel), false);
            }
        }
#ifdef MEADOWLARK_HAVE_PLUGINS
        pluginHost.idle();
#endif

        if (!fontApplied && elapsed >= 1.0)
        {
            fontApplied = true;
            ThemeManager::scanThemesAndFonts();
            const char* const* names = ThemeManager::getFontNames();
            int index = -1;
            for (int n = 0; n < ThemeManager::getFontCount(); n++)
            {
                if (ptFontName == names[n])
                    index = n;
            }
            const bool ok = index >= 0 && ThemeManager::applyFont(static_cast<uint8_t>(index));
            std::printf("meadowlark: font %s %s\n", ptFontName.c_str(), ok ? "applied" : "did not load");
        }

        if (!midiConfigured && elapsed >= 1.0)
        {
            midiConfigured = true;
            AudioLock::acquire();
            mtConfig.midi.clkOut = clockOut_usb_and_jack;
            mtConfig.midi.transportOut = clockOut_usb_and_jack;
            mtConfig.midi.notesOutMode = notesOut_mode_usb_and_jack;
            AudioLock::release();
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
    if (midiTrace && traceSink.clocks > 1)
    {
        // Clocks are counted on both ports.
        const double span = (traceSink.lastClockBlock - traceSink.firstClockBlock) * kAudioBlockSamples / kSampleRate;
        std::printf("  midi clocks          : %llu per port, %.2f per second\n",
                    static_cast<unsigned long long>(traceSink.clocks / 2),
                    (traceSink.clocks / 2 - 1) / span);
    }

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
