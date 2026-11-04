// Plugin host checks with the in-tree test synths (tests/plugins): both formats load, play
// what core sends on their channel and nothing else, mix with volume, pan and sends, save
// and restore their state through the workspace, and are found by --list-plugins'
// scanners; plugins.toml is read.
//
//   plugin_test <scratch dir> [clap file] [lv2 bundle dir]
//   plugin_test <scratch dir> --try clap:file[#id] | lv2:uri
//
// The defaults are the test synths in the build tree. --try checks an installed plugin
// instead: it loads, a note sounds, and its state saves and restores.

#include "plugins/PluginConfig.h"
#include "plugins/PluginHost.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
int failures = 0;

#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                   \
        }                                                                 \
    } while (0)

constexpr int kFrames = 128;
constexpr char kLv2Uri[] = "urn:meadowlark:test-synth";

struct Peaks
{
    int bus[ExternalInstruments::kBusCount] = {};
};

// Renders blocks, returning each bus's peak over them.
Peaks render(plugins::PluginHost& host, int blocks)
{
    Peaks peaks;
    int16_t data[ExternalInstruments::kBusCount][kFrames];
    int16_t* buses[ExternalInstruments::kBusCount];
    for (int bus = 0; bus < ExternalInstruments::kBusCount; bus++)
        buses[bus] = data[bus];
    for (int block = 0; block < blocks; block++)
    {
        host.render(buses, kFrames);
        for (int bus = 0; bus < ExternalInstruments::kBusCount; bus++)
            for (int f = 0; f < kFrames; f++)
                peaks.bus[bus] = std::max(peaks.bus[bus], std::abs(static_cast<int>(data[bus][f])));
    }
    return peaks;
}

bool near(int value, int expected)
{
    return std::abs(value - expected) <= expected / 50 + 2;
}

plugins::SlotConfig slotFor(const std::string& format, const std::string& clapFile, int channel)
{
    plugins::SlotConfig slot;
    slot.format = format;
    slot.location = format == "clap" ? clapFile : kLv2Uri;
    slot.channel = channel;
    return slot;
}

void checkFormat(const std::string& format, const std::string& clapFile, const std::string& scratch)
{
    std::printf("%s\n", format.c_str());
    const std::string root = scratch + "/" + format;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root + "/Workspace");

    plugins::PluginHost host;
    host.setCardRoot(root);
    std::string error;
    CHECK(host.add(slotFor(format, clapFile, 3), error));
    if (!error.empty())
        std::printf("  %s\n", error.c_str());
    if (host.size() != 1)
        return;
    plugins::Plugin* plugin = host.pluginOnChannel(3);
    CHECK(plugin != nullptr);
    CHECK(host.pluginOnChannel(1) == nullptr);
    CHECK(plugin && plugin->name() == "Meadowlark Test Synth");
    CHECK(plugin && plugin->id() == (format == "clap" ? "org.meadowlark.test-synth" : kLv2Uri));
    CHECK(plugin && !plugin->hasEditor(plugins::EditorParent::Api::kX11));
    CHECK(!host.isStateDirty());

    Midi* midi = host.getMidi();
    Peaks peaks = render(host, 2);
    CHECK(peaks.bus[ExternalInstruments::kDryLeft] == 0);

    // Another channel's note is not this plugin's.
    midi->sendNoteOn(69, 127, 1);
    peaks = render(host, 4);
    CHECK(peaks.bus[ExternalInstruments::kDryLeft] == 0);

    // Level 0.5 at full velocity, both sides, no sends.
    midi->sendNoteOn(69, 127, 3);
    peaks = render(host, 4);
    std::printf("  note: peak %d / %d\n", peaks.bus[ExternalInstruments::kDryLeft], peaks.bus[ExternalInstruments::kDryRight]);
    CHECK(near(peaks.bus[ExternalInstruments::kDryLeft], 16384));
    CHECK(near(peaks.bus[ExternalInstruments::kDryRight], 16384));
    CHECK(peaks.bus[ExternalInstruments::kDelayLeft] == 0);
    CHECK(peaks.bus[ExternalInstruments::kReverbRight] == 0);

    midi->sendNoteOff(69, 0, 3);
    render(host, 1);
    peaks = render(host, 2);
    CHECK(peaks.bus[ExternalInstruments::kDryLeft] == 0);

    // CC 7 sets the level, which is the state.
    midi->sendControlChange(7, 32, 3);
    midi->sendNoteOn(69, 127, 3);
    peaks = render(host, 4);
    const int quarter = 32767 * 32 / 127;
    std::printf("  level 32: peak %d\n", peaks.bus[ExternalInstruments::kDryLeft]);
    CHECK(near(peaks.bus[ExternalInstruments::kDryLeft], quarter));
    midi->sendNoteOff(69, 0, 3);
    render(host, 2);

    host.saveToWorkspace("Workspace");
    const std::string stateFile = root + "/Workspace/Plugins/" + plugins::PluginHost::stateFileName(3);
    CHECK(std::filesystem::exists(stateFile));
    std::printf("  state: %ju bytes\n", static_cast<uintmax_t>(std::filesystem::exists(stateFile) ? std::filesystem::file_size(stateFile) : 0));

    // Saving a project copies the state; opening one copies it back.
    std::filesystem::create_directories(root + "/Projects/song");
    host.copyProjectFiles("Workspace", "Projects/song");
    CHECK(std::filesystem::exists(root + "/Projects/song/Plugins/" + plugins::PluginHost::stateFileName(3)));

    // A new instance takes the saved level.
    {
        plugins::PluginHost other;
        other.setCardRoot(root);
        CHECK(other.add(slotFor(format, clapFile, 3), error));
        std::filesystem::remove_all(root + "/Workspace/Plugins");
        other.copyProjectFiles("Projects/song", "Workspace");
        other.loadFromWorkspace("Workspace");
        other.getMidi()->sendNoteOn(69, 127, 3);
        peaks = render(other, 4);
        std::printf("  restored: peak %d\n", peaks.bus[ExternalInstruments::kDryLeft]);
        CHECK(near(peaks.bus[ExternalInstruments::kDryLeft], quarter));
    }

    // Another plugin's state on the channel is left alone.
    {
        std::ofstream(stateFile, std::ios::binary) << "Meadowlark plugin state 1\n" << format << "\nsomething.else\nxx";
        plugins::PluginHost other;
        other.setCardRoot(root);
        CHECK(other.add(slotFor(format, clapFile, 3), error));
        other.loadFromWorkspace("Workspace");
        other.getMidi()->sendNoteOn(69, 127, 3);
        peaks = render(other, 4);
        CHECK(near(peaks.bus[ExternalInstruments::kDryLeft], 16384));
    }

    // Pan and sends: hard right, -6 dB, full reverb send, half delay send.
    {
        plugins::PluginHost other;
        plugins::SlotConfig slot = slotFor(format, clapFile, 16);
        slot.pan = 1.0f;
        slot.volumeDb = -6.0206f;
        slot.reverbSend = 1.0f;
        slot.delaySend = 0.5f;
        CHECK(other.add(slot, error));
        other.getMidi()->sendNoteOn(69, 127, 16);
        peaks = render(other, 4);
        CHECK(peaks.bus[ExternalInstruments::kDryLeft] == 0);
        CHECK(near(peaks.bus[ExternalInstruments::kDryRight], 8192));
        CHECK(peaks.bus[ExternalInstruments::kReverbLeft] == 0);
        CHECK(near(peaks.bus[ExternalInstruments::kReverbRight], 8192));
        CHECK(near(peaks.bus[ExternalInstruments::kDelayRight], 4096));
        // A second slot on a taken channel is refused.
        CHECK(!other.add(slot, error));
    }
}

// An installed plugin on channel 1.
void tryPlugin(const std::string& spec, const std::string& scratch)
{
    plugins::SlotConfig slot;
    const size_t colon = spec.find(':');
    slot.format = spec.substr(0, colon);
    slot.location = colon == std::string::npos ? "" : spec.substr(colon + 1);
    if (slot.format == "clap" && slot.location.find('#') != std::string::npos)
    {
        slot.id = slot.location.substr(slot.location.find('#') + 1);
        slot.location.resize(slot.location.find('#'));
    }
    slot.channel = 1;
    const std::string root = scratch + "/try";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root + "/Workspace");

    plugins::PluginHost host;
    host.setCardRoot(root);
    std::string error;
    CHECK(host.add(slot, error));
    if (host.size() == 0)
    {
        std::printf("  %s\n", error.c_str());
        return;
    }
    plugins::Plugin* plugin = host.pluginOnChannel(1);
    std::printf("%s (%s)\n", plugin->name().c_str(), plugin->id().c_str());
    std::printf("  editor: %s\n",
                plugin->hasEditor(plugins::EditorParent::Api::kX11) ? "X11"
                : plugin->hasEditor(plugins::EditorParent::Api::kCocoa) ? "Cocoa"
                                                                       : "none");
    render(host, 20);
    host.getMidi()->sendNoteOn(60, 110, 1);
    const auto start = std::chrono::steady_clock::now();
    Peaks peaks = render(host, 100);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("  note: peak %d, 100 blocks in %.0f ms (%.0f%% of real time)\n",
                peaks.bus[ExternalInstruments::kDryLeft],
                seconds * 1000,
                100 * seconds / (100 * kFrames / 44100.0));
    CHECK(peaks.bus[ExternalInstruments::kDryLeft] > 0);
    host.getMidi()->sendNoteOff(60, 0, 1);
    render(host, 50);

    host.saveToWorkspace("Workspace");
    const std::string stateFile = root + "/Workspace/Plugins/" + plugins::PluginHost::stateFileName(1);
    CHECK(std::filesystem::exists(stateFile));
    const uintmax_t size = std::filesystem::exists(stateFile) ? std::filesystem::file_size(stateFile) : 0;
    std::printf("  state: %ju bytes\n", size);
    host.loadFromWorkspace("Workspace");
    host.saveToWorkspace("Workspace");
    CHECK(std::filesystem::exists(stateFile) && std::filesystem::file_size(stateFile) == size);
}

void checkConfig(const std::string& scratch)
{
    std::printf("plugins.toml\n");
    const std::string path = scratch + "/plugins.toml";
    std::ofstream(path) << "[[plugin]]\nclap = \"~/x.clap\"\nid = \"a.b\"\nchannel = 2\npan = -0.5\nreverb_send = 0.25\n"
                           "[[plugin]]\nlv2 = \"urn:x\"\nchannel = 16\nvolume_db = -3\n"
                           "[[plugin]]\nlv2 = \"urn:y\"\nchannel = 2\n"            // channel taken
                           "[[plugin]]\nclap = \"a\"\nlv2 = \"b\"\nchannel = 3\n"  // both formats
                           "[[plugin]]\nlv2 = \"urn:z\"\nchannel = 17\n"           // no such channel
                           "[[plugin]]\nlv2 = \"urn:w\"\nchannel = 4\npan = 2\n";  // pan out of range
    plugins::PluginConfig config;
    CHECK(plugins::loadPluginConfig(path, config));
    CHECK(config.slots.size() == 2);
    if (config.slots.size() == 2)
    {
        const char* home = std::getenv("HOME");
        CHECK(config.slots[0].format == "clap");
        CHECK(config.slots[0].location == std::string(home ? home : "") + "/x.clap");
        CHECK(config.slots[0].id == "a.b");
        CHECK(config.slots[0].channel == 2);
        CHECK(config.slots[0].pan == -0.5f);
        CHECK(config.slots[0].reverbSend == 0.25f);
        CHECK(config.slots[1].format == "lv2");
        CHECK(config.slots[1].location == "urn:x");
        CHECK(config.slots[1].volumeDb == -3.0f);
    }

    // The default file is all comments.
    std::ofstream(path) << plugins::defaultPluginConfig();
    CHECK(plugins::loadPluginConfig(path, config));
    CHECK(config.slots.empty());
    std::ofstream(path) << "[[plugin]\n";
    CHECK(!plugins::loadPluginConfig(path, config));
    CHECK(plugins::loadPluginConfig(scratch + "/missing.toml", config));
}

void checkScanners(const std::string& clapFile)
{
    std::printf("scanners\n");
    bool clapFound = false;
    for (const plugins::Found& found : plugins::findClapPlugins())
        clapFound = clapFound || (found.location == clapFile && found.id == "org.meadowlark.test-synth");
    CHECK(clapFound);
    bool lv2Found = false;
    for (const plugins::Found& found : plugins::findLv2Plugins())
        lv2Found = lv2Found || found.location == kLv2Uri;
    CHECK(lv2Found);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: plugin_test <scratch dir> [clap file] [lv2 bundle dir]\n");
        return 2;
    }
    const std::string scratch = argv[1];
    if (argc == 4 && std::strcmp(argv[2], "--try") == 0)
    {
        std::filesystem::create_directories(scratch);
        tryPlugin(argv[3], scratch);
        std::printf(failures ? "%d check(s) failed\n" : "plugin works\n", failures);
        return failures ? 1 : 0;
    }
    const std::string clapFile = std::filesystem::absolute(argc > 2 ? argv[2] : TEST_CLAP_FILE).string();
    const std::string lv2Dir = std::filesystem::absolute(argc > 3 ? argv[3] : TEST_LV2_DIR).string();
    std::filesystem::create_directories(scratch);
    // Before lilv looks: only the test bundle, and the scanner finds the test .clap.
    setenv("LV2_PATH", lv2Dir.c_str(), 1);
    setenv("CLAP_PATH", std::filesystem::path(clapFile).parent_path().c_str(), 1);

    checkConfig(scratch);
    checkFormat("clap", clapFile, scratch);
    checkFormat("lv2", clapFile, scratch);
    checkScanners(clapFile);

    if (failures)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all plugin checks passed\n");
    return 0;
}
