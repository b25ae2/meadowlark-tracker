#pragma once

// midi.toml: which system MIDI ports the firmware's "usb" and "jack" ports connect to.
//
// Like input.toml, the defaults are a TOML document (defaultMidiConfig()), printed by
// `meadowlark --print-midi-config`; a user file only needs the settings it changes.

#include "midi/MidiPorts.h"

#include <string>
#include <vector>

namespace midi
{
struct PortConfig
{
    // Patterns for the system ports to connect: case-insensitive parts of their names, or
    // "*" for every hardware port.
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
};

struct MidiConfig
{
    bool enabled = true;
    PortConfig ports[kPortCount];
    // Added to every outgoing message's time, to line MIDI up with the sound (negative:
    // earlier, for synths that are slow to respond).
    double outputOffsetMs = 0.0;
};

const char* defaultMidiConfig();

// $MEADOWLARK_MIDI_CONFIG, else midi.toml next to input.toml.
std::string defaultMidiConfigPath();

// The defaults, then the file at path if it exists. Problems are reported on stderr and
// skipped; returns false when the file exists but is not valid TOML.
bool loadMidiConfig(const std::string& path, MidiConfig& config);

// Whether a system port matches one pattern. "*" matches hardware ports only, so it never
// connects Meadowlark to itself or to other programs.
bool matchesPattern(const std::string& pattern, const std::string& name, bool hardware);
} // namespace midi
