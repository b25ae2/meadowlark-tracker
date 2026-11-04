#include "plugins/PluginHost.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace plugins
{
namespace
{
constexpr char kStateMagic[] = "Meadowlark plugin state 1\n";
// More than a block's worth is dropped (a stuck queue, an export with the audio thread away).
constexpr size_t kMaxQueuedEvents = 1024;

int16_t toSample(float value)
{
    const float scaled = value * 32767.0f;
    if (scaled >= 32767.0f)
        return 32767;
    if (scaled <= -32768.0f)
        return -32768;
    return static_cast<int16_t>(std::lrintf(scaled));
}
} // namespace

struct PluginHost::Slot
{
    SlotConfig config;
    std::unique_ptr<Plugin> plugin;
    // Left and right gains for the dry signal and each send.
    float dry[2] = {};
    float delay[2] = {};
    float reverb[2] = {};

    std::mutex queueMutex;
    std::vector<MidiEvent> queued;
    std::vector<MidiEvent> block; // audio thread
    float left[kMaxFrames] = {};
    float right[kMaxFrames] = {};
};

PluginHost::PluginHost() = default;
PluginHost::~PluginHost() = default;

int PluginHost::load(const PluginConfig& config)
{
    int loaded = 0;
    for (const SlotConfig& slot : config.slots)
    {
        std::string error;
        if (add(slot, error))
            loaded++;
        else
            std::fprintf(stderr, "meadowlark: plugin on channel %d: %s\n", slot.channel, error.c_str());
    }
    return loaded;
}

bool PluginHost::add(const SlotConfig& config, std::string& error)
{
    if (config.channel < 1 || config.channel > 16 || byChannel[config.channel])
    {
        error = "channel " + std::to_string(config.channel) + " is taken or out of range";
        return false;
    }
    auto slot = std::make_unique<Slot>();
    slot->config = config;
    slot->plugin = config.format == "clap" ? loadClapPlugin(config.location, config.id, error)
                                           : loadLv2Plugin(config.location, error);
    if (!slot->plugin)
        return false;

    // Balance: the far side fades out, the near one stays at full level.
    const float volume = std::pow(10.0f, config.volumeDb / 20.0f);
    const float pan[2] = { std::min(1.0f, 1.0f - config.pan), std::min(1.0f, 1.0f + config.pan) };
    for (int side = 0; side < 2; side++)
    {
        slot->dry[side] = volume * pan[side];
        slot->delay[side] = slot->dry[side] * config.delaySend;
        slot->reverb[side] = slot->dry[side] * config.reverbSend;
    }
    slot->queued.reserve(kMaxQueuedEvents);
    slot->block.reserve(kMaxQueuedEvents);

    std::printf("meadowlark: plugin %s (%s) on channel %d\n",
                slot->plugin->name().c_str(),
                config.format.c_str(),
                config.channel);
    byChannel[config.channel] = slot.get();
    slots.push_back(std::move(slot));
    return true;
}

Plugin* PluginHost::pluginOnChannel(int channel)
{
    return channel >= 1 && channel <= 16 && byChannel[channel] ? byChannel[channel]->plugin.get() : nullptr;
}

void PluginHost::idle()
{
    for (auto& slot : slots)
        slot->plugin->idle();
}

void PluginHost::render(int16_t* buses[kBusCount], int frames)
{
    if (frames > static_cast<int>(kMaxFrames))
        frames = static_cast<int>(kMaxFrames);
    float mix[kBusCount][kMaxFrames] = {};
    for (auto& slot : slots)
    {
        // Events wait for the next block if the queue is busy. Swapping hands the queue the
        // last block's (empty) vector, so neither side allocates.
        slot->block.clear();
        {
            std::unique_lock<std::mutex> lock(slot->queueMutex, std::try_to_lock);
            if (lock.owns_lock())
                slot->block.swap(slot->queued);
        }
        slot->plugin->process(slot->block.data(), slot->block.size(), slot->left, slot->right, static_cast<uint32_t>(frames));

        const float* in[2] = { slot->left, slot->right };
        for (int side = 0; side < 2; side++)
        {
            for (int f = 0; f < frames; f++)
            {
                const float sample = in[side][f];
                mix[kDryLeft + side][f] += sample * slot->dry[side];
                mix[kDelayLeft + side][f] += sample * slot->delay[side];
                mix[kReverbLeft + side][f] += sample * slot->reverb[side];
            }
        }
    }
    for (int bus = 0; bus < kBusCount; bus++)
    {
        for (int f = 0; f < frames; f++)
            buses[bus][f] = toSample(mix[bus][f]);
    }
}

void PluginHost::queue(uint8_t channel, uint8_t status, uint8_t data1, uint8_t data2, uint8_t size)
{
    Slot* slot = channel >= 1 && channel <= 16 ? byChannel[channel] : nullptr;
    if (!slot)
        return;
    MidiEvent event;
    event.data[0] = static_cast<uint8_t>(status | ((channel - 1) & 0x0F));
    event.data[1] = data1 & 0x7F;
    event.data[2] = data2 & 0x7F;
    event.size = size;
    std::lock_guard<std::mutex> lock(slot->queueMutex);
    if (slot->queued.size() < kMaxQueuedEvents)
        slot->queued.push_back(event);
}

void PluginHost::sendControlChange(uint8_t index, uint8_t value, uint8_t channel)
{
    queue(channel, 0xB0, index, value, 3);
}

void PluginHost::sendProgramChange(uint8_t programNumber, uint8_t channel)
{
    queue(channel, 0xC0, programNumber, 0, 2);
}

void PluginHost::sendAfterTouch(uint8_t velocity, uint8_t channel)
{
    queue(channel, 0xD0, velocity, 0, 2);
}

void PluginHost::sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel)
{
    queue(channel, 0xA0, note, value, 3);
}

void PluginHost::sendPitchBend(int value, uint8_t channel)
{
    value = std::max(-8192, std::min(8191, value)) + 8192;
    queue(channel, 0xE0, static_cast<uint8_t>(value & 0x7F), static_cast<uint8_t>(value >> 7), 3);
}

void PluginHost::sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel)
{
    // Velocity 0 is a note off in MIDI; keep the note on audible.
    queue(channel, 0x90, note, velocity ? velocity : 1, 3);
}

void PluginHost::sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel)
{
    queue(channel, 0x80, note, velocity, 3);
}

std::string PluginHost::stateFileName(int channel)
{
    char name[32];
    std::snprintf(name, sizeof(name), "channel-%02d.state", channel);
    return name;
}

std::string PluginHost::hostPath(const char* cardPath) const
{
    std::string path = cardRoot;
    if (cardPath[0] != '/')
        path += '/';
    return path + cardPath;
}

bool PluginHost::isStateDirty()
{
    if (cardRoot.empty())
        return false;
    for (auto& slot : slots)
    {
        if (slot->plugin->isStateDirty())
            return true;
    }
    return false;
}

void PluginHost::saveToWorkspace(const char* workspacePath)
{
    if (cardRoot.empty() || slots.empty())
        return;
    const std::filesystem::path dir = hostPath(workspacePath) + "/Plugins";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    for (auto& slot : slots)
    {
        std::vector<uint8_t> state;
        if (!slot->plugin->saveState(state))
        {
            std::fprintf(stderr, "meadowlark: %s did not save its state\n", slot->plugin->name().c_str());
            continue;
        }
        const std::filesystem::path file = dir / stateFileName(slot->config.channel);
        const std::filesystem::path temporary = file.string() + ".new";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << kStateMagic << slot->config.format << '\n' << slot->plugin->id() << '\n';
            out.write(reinterpret_cast<const char*>(state.data()), static_cast<std::streamsize>(state.size()));
            if (!out.good())
            {
                std::fprintf(stderr, "meadowlark: cannot write %s\n", temporary.c_str());
                continue;
            }
        }
        std::filesystem::rename(temporary, file, error);
        if (error)
            std::fprintf(stderr, "meadowlark: cannot write %s\n", file.c_str());
    }
}

void PluginHost::loadFromWorkspace(const char* workspacePath)
{
    if (cardRoot.empty())
        return;
    const std::string dir = hostPath(workspacePath) + "/Plugins/";
    for (auto& slot : slots)
    {
        std::ifstream in(dir + stateFileName(slot->config.channel), std::ios::binary);
        if (!in)
            continue;
        std::string magic;
        std::string format;
        std::string id;
        std::getline(in, magic);
        std::getline(in, format);
        std::getline(in, id);
        if (magic + '\n' != kStateMagic || format != slot->config.format || id != slot->plugin->id())
        {
            // Another plugin's state: this one stays as it is.
            std::printf("meadowlark: the project's channel %d state is for %s, not %s\n",
                        slot->config.channel,
                        id.c_str(),
                        slot->plugin->id().c_str());
            continue;
        }
        const std::vector<uint8_t> state((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!slot->plugin->loadState(state))
            std::fprintf(stderr, "meadowlark: %s did not take the project's state\n", slot->plugin->name().c_str());
    }
}

void PluginHost::copyProjectFiles(const char* sourcePath, const char* destinationPath)
{
    if (cardRoot.empty())
        return;
    const std::filesystem::path source = hostPath(sourcePath) + "/Plugins";
    const std::filesystem::path destination = hostPath(destinationPath) + "/Plugins";
    std::error_code error;
    if (std::filesystem::equivalent(source, destination, error))
        return;
    std::filesystem::remove_all(destination, error);
    if (!std::filesystem::is_directory(source, error))
        return;
    std::filesystem::copy(source, destination, std::filesystem::copy_options::recursive, error);
    if (error)
        std::fprintf(stderr, "meadowlark: cannot copy %s: %s\n", source.c_str(), error.message().c_str());
}
} // namespace plugins
