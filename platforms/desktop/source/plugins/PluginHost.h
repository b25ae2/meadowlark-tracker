#pragma once

// The plugin host: core's external instruments (Platform.h). Each loaded plugin listens on
// one MIDI channel, so the tracker's MIDI instrument on that channel plays it; the host
// renders them all for every audio block and mixes them, with their volume, pan and sends,
// into core's buses.
//
// Threads: core's MIDI arrives on whichever thread core sends it from (the audio thread for
// the sequencer, the main loop for previews) and is queued per plugin; render() runs on the
// audio thread, with the main loop locked out (AudioLock). The rest is main loop.
//
// State: saving the project to the workspace writes each plugin's state to
// Workspace/Plugins/channel-NN.state; opening or saving a project copies the Plugins folder
// along with the project's other files. A project without a state for a plugin leaves the
// plugin as it is.

#include "Platform.h"
#include "plugins/Plugin.h"
#include "plugins/PluginConfig.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace plugins
{
class PluginHost : public ExternalInstruments, private Midi
{
  public:
    PluginHost();
    ~PluginHost();

    // Loads every plugin in config; ones that fail are reported and left out. Returns how
    // many were loaded. Main loop, before core starts.
    int load(const PluginConfig& config);
    // Adds one plugin (the tests' way in); false with error set if it can't load.
    bool add(const SlotConfig& slot, std::string& error);
    // The SD card's directory on the host, which core's paths are relative to; empty: no
    // card, so no state is saved.
    void setCardRoot(const std::string& hostRoot) { cardRoot = hostRoot; }

    size_t size() const { return slots.size(); }
    // nullptr when the channel (1 to 16) has none.
    Plugin* pluginOnChannel(int channel);

    // Main loop: the plugins' main-thread work.
    void idle();

    // ExternalInstruments
    Midi* getMidi() override { return this; }
    void render(int16_t* buses[kBusCount], int frames) override;
    void saveToWorkspace(const char* workspacePath) override;
    bool isStateDirty() override;
    void loadFromWorkspace(const char* workspacePath) override;
    void copyProjectFiles(const char* sourcePath, const char* destinationPath) override;

    // The state file of the plugin on a channel, under a project or workspace directory.
    static std::string stateFileName(int channel);

  private:
    struct Slot;

    // Midi: what core sends to MIDI instruments.
    bool isReadAvailable() override { return false; }
    void read() override {}
    void sendRealTime(MessageType) override {}
    void sendControlChange(uint8_t index, uint8_t value, uint8_t channel) override;
    void sendProgramChange(uint8_t programNumber, uint8_t channel) override;
    void sendAfterTouch(uint8_t velocity, uint8_t channel) override;
    void sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel) override;
    void sendPitchBend(int value, uint8_t channel) override;
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void queue(uint8_t channel, uint8_t status, uint8_t data1, uint8_t data2, uint8_t size);

    std::string hostPath(const char* cardPath) const;

    std::vector<std::unique_ptr<Slot>> slots;
    Slot* byChannel[17] = {};
    std::string cardRoot;
};
} // namespace plugins
