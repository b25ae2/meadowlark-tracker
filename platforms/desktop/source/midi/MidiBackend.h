#pragma once

// The system's MIDI: the ALSA sequencer on Linux, CoreMIDI on macOS.
//
// start() creates Meadowlark's own ports, one input and one output for each of the
// firmware's ports, which other programs can connect to. connectInput/connectOutput connect
// them to system ports as well; MidiConnector does that from midi.toml.

#include "midi/MidiConfig.h"
#include "midi/MidiPorts.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace midi
{
struct Endpoint
{
    uint64_t id = 0;   // backend's handle, unique while the port exists
    std::string name;  // "Client:Port" on ALSA, the display name on CoreMIDI
    bool hardware = false;
    bool own = false;  // one of Meadowlark's ports
};

class MidiBackend
{
  public:
    // Called on a backend thread for every message received on one of the ports.
    using Receive = std::function<void(int port, const Message& message)>;

    virtual ~MidiBackend() = default;
    virtual const char* name() const = 0;
    // Returns false, with a message on stderr, if the system's MIDI isn't available.
    virtual bool start(Receive receive) = 0;
    virtual void stop() = 0;

    // Ports that send MIDI (to connect to an input) and that receive it.
    virtual std::vector<Endpoint> sources() = 0;
    virtual std::vector<Endpoint> destinations() = 0;
    virtual bool connectInput(int port, const Endpoint& source) = 0;
    virtual bool connectOutput(int port, const Endpoint& destination) = 0;

    // To our output port's connections. One thread at a time (MidiScheduler's).
    virtual void send(int port, const Message& message) = 0;
};

// The platform's backend, or nullptr if this build has none.
std::unique_ptr<MidiBackend> createMidiBackend();

// Connects the ports to the system ports midi.toml names, now and as devices appear.
class MidiConnector
{
  public:
    MidiConnector(MidiBackend& backend, const MidiConfig& config) : backend(backend), config(config) {}

    // Connects what matches and isn't connected yet. Cheap enough to call every second.
    void update();

  private:
    void connect(int port, bool input, const std::vector<Endpoint>& endpoints);

    MidiBackend& backend;
    MidiConfig config;
    // Connected endpoints by direction and port; ones that disappear are forgotten, so a
    // device plugged back in is connected again.
    std::set<std::pair<uint64_t, std::string>> connected[2][kPortCount];
};

// Prints the system's ports and which patterns of the config match them.
void printMidiPorts(MidiBackend& backend, const MidiConfig& config);
} // namespace midi
