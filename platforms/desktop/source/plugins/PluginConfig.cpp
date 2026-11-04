#include "PluginConfig.h"

#include "ConfigPaths.h"

#include <toml++/toml.hpp>

#include <cstdio>
#include <sys/stat.h>

namespace plugins
{
namespace
{
constexpr char kDefaultConfig[] = R"toml(# Instrument plugins (CLAP and LV2), played by the tracker's MIDI instruments.
#
# Each [[plugin]] is loaded at startup and hears the MIDI instrument on its channel: what
# the tracker sends to MIDI instrument 1 goes to the plugin on channel 1, and so on, whatever
# Config's MIDI out is set to, and also while a song or pattern is exported. The plugins'
# sound joins the tracks: into the master mix, with sends into the delay and reverb.
#
# Their settings are saved with the project (the project's Plugins folder). The plugin's
# own editor opens in a window of its own with the plugin_editor action (F10), for the
# instrument selected in the tracker; editors made for GTK or Qt (LV2) can't be shown.
#
# meadowlark --list-plugins shows the instruments installed on this system.
#
# [[plugin]]
# clap = "~/.clap/Dexed.clap"   # a .clap file (a bundle on macOS)
# id = "com.digital-suburban.dexed"   # optional: the plugin in the file, if it has several
# channel = 1
# volume_db = 0.0     # optional, -60 to +12
# pan = 0.0           # optional, -1 (left) to 1 (right)
# delay_send = 0.0    # optional, 0 to 1
# reverb_send = 0.0   # optional, 0 to 1
#
# [[plugin]]
# lv2 = "http://drobilla.net/plugins/mda/EPiano"   # the plugin's URI
# channel = 2
)toml";

void warn(const toml::node& node, const std::string& message)
{
    const toml::source_region& where = node.source();
    std::fprintf(stderr,
                 "%s:%u: %s\n",
                 where.path ? where.path->c_str() : "plugins.toml",
                 static_cast<unsigned>(where.begin.line),
                 message.c_str());
}

bool readNumber(const toml::table& table, const char* key, double low, double high, float& out)
{
    const toml::node* value = table.get(key);
    if (!value)
        return true;
    std::optional<double> number = value->value<double>();
    if (!number || *number < low || *number > high)
    {
        char message[96];
        std::snprintf(message, sizeof(message), "%s must be a number from %g to %g", key, low, high);
        warn(*value, message);
        return false;
    }
    out = static_cast<float>(*number);
    return true;
}

bool readSlot(const toml::table& table, SlotConfig& slot)
{
    static const char* const kKnown[] = { "clap", "lv2", "id", "channel", "volume_db", "pan", "delay_send", "reverb_send" };
    for (auto&& [key, value] : table)
    {
        bool known = false;
        for (const char* name : kKnown)
            known = known || key.str() == name;
        if (!known)
            warn(value, "unknown setting \"" + std::string(key.str()) + "\"");
    }

    std::optional<std::string> clap = table["clap"].value<std::string>();
    std::optional<std::string> lv2 = table["lv2"].value<std::string>();
    if (clap.has_value() == lv2.has_value())
    {
        warn(table, "a [[plugin]] needs either clap = \"file\" or lv2 = \"uri\"");
        return false;
    }
    slot.format = clap ? "clap" : "lv2";
    slot.location = clap ? expandHome(*clap) : *lv2;
    slot.id = table["id"].value_or(std::string());

    std::optional<int64_t> channel = table["channel"].value<int64_t>();
    if (!channel || *channel < 1 || *channel > 16)
    {
        warn(table, "a [[plugin]] needs a channel from 1 to 16");
        return false;
    }
    slot.channel = static_cast<int>(*channel);
    return readNumber(table, "volume_db", -60, 12, slot.volumeDb) && readNumber(table, "pan", -1, 1, slot.pan) &&
           readNumber(table, "delay_send", 0, 1, slot.delaySend) && readNumber(table, "reverb_send", 0, 1, slot.reverbSend);
}

bool fileExists(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}
} // namespace

const char* defaultPluginConfig()
{
    return kDefaultConfig;
}

std::string defaultPluginConfigPath()
{
    return userConfigPath("plugins.toml", "MEADOWLARK_PLUGIN_CONFIG");
}

bool loadPluginConfig(const std::string& path, PluginConfig& config)
{
    config = PluginConfig{};
    if (path.empty() || !fileExists(path))
        return true;
    toml::parse_result result = toml::parse_file(path);
    if (!result)
    {
        const toml::parse_error& error = result.error();
        std::fprintf(stderr,
                     "%s:%u: %s (no plugins loaded)\n",
                     path.c_str(),
                     static_cast<unsigned>(error.source().begin.line),
                     std::string(error.description()).c_str());
        return false;
    }
    for (auto&& [key, value] : result.table())
    {
        if (key.str() != "plugin")
            warn(value, "unknown setting \"" + std::string(key.str()) + "\"");
    }
    if (const toml::node* entries = result.table().get("plugin"))
    {
        const toml::array* list = entries->as_array();
        if (!list)
        {
            warn(*entries, "plugins are [[plugin]] tables");
            return true;
        }
        for (const toml::node& entry : *list)
        {
            SlotConfig slot;
            const toml::table* table = entry.as_table();
            if (!table || !readSlot(*table, slot))
                continue;
            bool taken = false;
            for (const SlotConfig& other : config.slots)
                taken = taken || other.channel == slot.channel;
            if (taken)
                warn(entry, "channel " + std::to_string(slot.channel) + " already has a plugin");
            else
                config.slots.push_back(slot);
        }
    }
    return true;
}
} // namespace plugins
