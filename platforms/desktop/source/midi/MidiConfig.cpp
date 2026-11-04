#include "MidiConfig.h"

#include "ConfigPaths.h"

#include <toml++/toml.hpp>

#include <cctype>
#include <cstdio>
#include <sys/stat.h>

namespace midi
{
namespace
{
constexpr char kDefaultConfig[] = R"toml(# Meadowlark MIDI ports.
#
# The firmware has two MIDI ports: "usb" (the Tracker Mini's USB MIDI) and "jack" (its MIDI
# in/out jack). Config > MIDI chooses which of them notes, CC, clock and transport use; all
# of that is off until you turn it on there.
#
# Each port shows up for other programs as "Meadowlark USB" / "Meadowlark Jack" (ALSA:
# client "Meadowlark", ports "USB" and "Jack"), with an input and an output, so it can be
# connected by hand (aconnect, qpwgraph, Audio MIDI Setup's routing in a DAW). It also
# connects itself to the system ports listed here, and to matching devices plugged in later.
# A pattern is part of a port's name, any case ("launchkey", "Midi Through:Port-0"), or "*"
# for every hardware port. `meadowlark --list-midi` prints the names.

enabled = true

[usb]
inputs = ["*"]    # controllers and keyboards play the tracker
outputs = []

[jack]
inputs = []
outputs = []

[timing]
# Outgoing messages follow the audio's latency so notes and clock line up with the sound.
# This moves them by the given milliseconds (negative: earlier).
output_offset_ms = 0
)toml";

void warn(const toml::node& node, const std::string& message)
{
    const toml::source_region& where = node.source();
    std::fprintf(stderr,
                 "%s:%u: %s\n",
                 where.path ? where.path->c_str() : "midi.toml",
                 static_cast<unsigned>(where.begin.line),
                 message.c_str());
}

void readPatterns(const toml::table& section, const char* key, std::vector<std::string>& out)
{
    const toml::node* node = section.get(key);
    if (!node)
        return;
    const toml::array* list = node->as_array();
    if (!list)
    {
        warn(*node, std::string(key) + " must be a list of names");
        return;
    }
    out.clear();
    for (const toml::node& entry : *list)
    {
        if (std::optional<std::string> name = entry.value<std::string>())
            out.push_back(*name);
        else
            warn(entry, std::string(key) + " entries must be strings");
    }
}

void readConfig(const toml::table& root, MidiConfig& config)
{
    if (const toml::node* node = root.get("enabled"))
    {
        if (std::optional<bool> value = node->value<bool>())
            config.enabled = *value;
        else
            warn(*node, "enabled must be true or false");
    }
    for (int port = 0; port < kPortCount; port++)
    {
        const toml::node* node = root.get(portName(port));
        if (!node)
            continue;
        if (const toml::table* section = node->as_table())
        {
            readPatterns(*section, "inputs", config.ports[port].inputs);
            readPatterns(*section, "outputs", config.ports[port].outputs);
        }
        else
            warn(*node, std::string("[") + portName(port) + "] must be a table");
    }
    if (const toml::node* node = root.at_path("timing.output_offset_ms").node())
    {
        if (std::optional<double> value = node->value<double>())
            config.outputOffsetMs = *value;
        else
            warn(*node, "timing.output_offset_ms must be a number");
    }
    for (auto&& [key, value] : root)
    {
        const std::string_view name = key.str();
        if (name != "enabled" && name != "usb" && name != "jack" && name != "timing")
            warn(value, "unknown setting \"" + std::string(name) + "\"");
    }
}

bool fileExists(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

std::string lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
} // namespace

const char* defaultMidiConfig()
{
    return kDefaultConfig;
}

std::string defaultMidiConfigPath()
{
    return userConfigPath("midi.toml", "MEADOWLARK_MIDI_CONFIG");
}

bool loadMidiConfig(const std::string& path, MidiConfig& config)
{
    config = MidiConfig{};
    toml::parse_result defaults = toml::parse(std::string_view(kDefaultConfig), std::string_view("(defaults)"));
    if (!defaults)
    {
        std::fprintf(stderr, "midi defaults: %s\n", std::string(defaults.error().description()).c_str());
        return false;
    }
    readConfig(defaults.table(), config);

    if (path.empty() || !fileExists(path))
        return true;
    toml::parse_result result = toml::parse_file(path);
    if (!result)
    {
        const toml::parse_error& error = result.error();
        std::fprintf(stderr,
                     "%s:%u: %s (using the default MIDI settings)\n",
                     path.c_str(),
                     static_cast<unsigned>(error.source().begin.line),
                     std::string(error.description()).c_str());
        return false;
    }
    readConfig(result.table(), config);
    return true;
}

bool matchesPattern(const std::string& pattern, const std::string& name, bool hardware)
{
    if (pattern == "*")
        return hardware;
    return !pattern.empty() && lower(name).find(lower(pattern)) != std::string::npos;
}
} // namespace midi
