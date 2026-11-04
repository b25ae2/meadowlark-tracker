#pragma once

// plugins.toml: instrument plugins to load at startup, each played by one of the tracker's
// MIDI instruments (MIDI channel 1 to 16).

#include <string>
#include <vector>

namespace plugins
{
struct SlotConfig
{
    std::string format;   // "clap" or "lv2"
    std::string location; // the .clap path or the LV2 URI
    std::string id;       // CLAP: the plugin in the file, empty for the first
    int channel = 1;
    float volumeDb = 0.0f;
    float pan = 0.0f;        // -1 left .. 1 right
    float delaySend = 0.0f;  // 0 .. 1
    float reverbSend = 0.0f; // 0 .. 1
};

struct PluginConfig
{
    std::vector<SlotConfig> slots;
};

const char* defaultPluginConfig();

// $MEADOWLARK_PLUGIN_CONFIG, else plugins.toml next to input.toml.
std::string defaultPluginConfigPath();

// The file at path if it exists (no file: no plugins). Problems are reported on stderr and
// the entry skipped; returns false when the file is not valid TOML.
bool loadPluginConfig(const std::string& path, PluginConfig& config);
} // namespace plugins
