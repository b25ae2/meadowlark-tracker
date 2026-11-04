#include "InputConfig.h"

#include "Actions.h"

#include <toml++/toml.hpp>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <sys/stat.h>

namespace input
{
namespace
{
constexpr char kDefaultConfig[] = R"toml(# Meadowlark input bindings.
#
# Each action takes a list of bindings; any of them triggers it. A binding is an input name,
# or a list of input names for a chord: all held together, triggered by the last one pressed
# (that input's own bindings are skipped while the chord holds it).
#
# Inputs:
#   key:<name>        keyboard key by position, SDL scancode names: "key:A", "key:Space",
#                     "key:Return", "key:Left Shift", "key:Up", "key:F1", "key:Keypad +"
#   pad:<button>      gamepad button: a b x y back guide start leftstick rightstick
#                     leftshoulder rightshoulder dpup dpdown dpleft dpright misc1 ...
#   pad:<axis>+ / -   gamepad axis direction: leftx lefty rightx righty;
#                     pad:lefttrigger, pad:righttrigger
#   joy:button<n>     raw joystick: joy:button0, joy:axis1+, joy:axis1-, joy:hat0up/down/left/right
#   mouse:<button>    left middle right x1 x2
#   mouse:wheelup     wheelup wheeldown wheelleft wheelright, one tap per wheel step
#   mouse:moveup      moveup movedown moveleft moveright, one tap per motion_step pixels
#                     (trackballs)
#
# `meadowlark --trace-input` prints the name of every input you press.
#
# Actions, the Tracker Mini's buttons:
#   screen1 ... screen8 (the buttons under the display, for the labels along its bottom)
#   play rec shift copy insert delete up down left right
#   note instr fx1 fx2 (= vol) pattern menu middle fn1 fn2 fn3 fn4 plus10 plus1 minus1 minus10
#   button0 ... button43 (any interface button by index)
# and the desktop ones: power (hold to save and quit), fullscreen, screenshot, quit.
#
# A file only needs the actions it changes; the others keep these defaults unless
# options.defaults is false. An empty list unbinds an action.

[options]
defaults = true
axis_press = 0.5       # axis travel (0..1) that presses a direction
axis_release = 0.35    # and that releases it again
motion_step = 40       # pixels of pointer motion per mouse:move* tap
capture_mouse = false  # keep the pointer in the window (relative mode), for mouse:move*

[bindings]
up      = ["key:Up"]
down    = ["key:Down"]
left    = ["key:Left"]
right   = ["key:Right"]
shift   = ["key:Left Shift", "key:Right Shift"]

play    = ["key:Space"]
rec     = ["key:R"]
copy    = ["key:C"]
insert  = ["key:Insert", "key:I"]
delete  = ["key:Delete", "key:Backspace"]

note    = ["key:1"]
instr   = ["key:2"]
fx1     = ["key:3"]
fx2     = ["key:4"]
pattern = ["key:5"]

menu    = ["key:Escape"]
middle  = ["key:Return"]
fn1     = ["key:6"]
fn2     = ["key:7"]
fn3     = ["key:8"]
fn4     = ["key:9"]

# The buttons under the screen, left to right like its labels.
screen1 = ["key:F1"]
screen2 = ["key:F2"]
screen3 = ["key:F3"]
screen4 = ["key:F4"]
screen5 = ["key:F5"]
screen6 = ["key:F6"]
screen7 = ["key:F7"]
screen8 = ["key:F8"]

plus10  = ["key:PageUp"]
minus10 = ["key:PageDown"]
plus1   = ["key:=", "key:Keypad +"]
minus1  = ["key:-", "key:Keypad -"]

power      = []
fullscreen = ["key:F11"]
screenshot = ["key:F12"]
quit       = [["key:Left Ctrl", "key:Q"]]
)toml";

using BindingMap = std::map<ActionId, std::vector<std::vector<InputId>>>;

struct Loader
{
    std::string source;
    bool ok = true;

    void warn(const toml::node& node, const std::string& message)
    {
        std::fprintf(stderr, "%s:%u: %s\n", source.c_str(), node.source().begin.line, message.c_str());
        ok = false;
    }

    bool parseOneInput(const toml::node& node, InputId& out)
    {
        std::optional<std::string> name = node.value<std::string>();
        if (!name)
        {
            warn(node, "expected an input name string");
            return false;
        }
        if (!parseInput(*name, out))
        {
            warn(node, "unknown input \"" + *name + "\"");
            return false;
        }
        return true;
    }

    // Reads [bindings]; each action listed replaces that action's bindings in the map.
    void readBindings(const toml::table& root, BindingMap& map)
    {
        const toml::node_view<const toml::node> section = root["bindings"];
        if (!section)
            return;
        const toml::table* table = section.as_table();
        if (!table)
        {
            warn(*section.node(), "[bindings] must be a table");
            return;
        }

        for (auto&& [key, value] : *table)
        {
            ActionId action = 0;
            if (!parseAction(key.str(), action))
            {
                warn(value, "unknown action \"" + std::string(key.str()) + "\"");
                continue;
            }
            const toml::array* list = value.as_array();
            if (!list)
            {
                warn(value, "bindings for \"" + std::string(key.str()) + "\" must be a list");
                continue;
            }

            std::vector<std::vector<InputId>>& bindings = map[action];
            bindings.clear();
            for (const toml::node& entry : *list)
            {
                std::vector<InputId> inputs;
                if (const toml::array* chord = entry.as_array())
                {
                    for (const toml::node& part : *chord)
                    {
                        InputId input = 0;
                        if (parseOneInput(part, input))
                            inputs.push_back(input);
                    }
                    if (inputs.size() != chord->size() || inputs.empty())
                        inputs.clear();
                }
                else
                {
                    InputId input = 0;
                    if (parseOneInput(entry, input))
                        inputs.push_back(input);
                }
                if (!inputs.empty())
                    bindings.push_back(std::move(inputs));
            }
        }
    }

    void readOptions(const toml::table& root, InputConfig& config)
    {
        const toml::node_view<const toml::node> options = root["options"];
        auto readFloat = [&](const char* name, float& out) {
            const toml::node_view<const toml::node> node = options[name];
            if (!node)
                return;
            if (std::optional<double> value = node.value<double>())
                out = static_cast<float>(*value);
            else
                warn(*node.node(), std::string("options.") + name + " must be a number");
        };
        readFloat("axis_press", config.options.axisPress);
        readFloat("axis_release", config.options.axisRelease);
        readFloat("motion_step", config.options.motionStep);
        if (const toml::node_view<const toml::node> node = options["capture_mouse"])
        {
            if (std::optional<bool> value = node.value<bool>())
                config.captureMouse = *value;
            else
                warn(*node.node(), "options.capture_mouse must be true or false");
        }
    }
};

bool fileExists(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}
} // namespace

const char* defaultInputConfig()
{
    return kDefaultConfig;
}

std::string defaultInputConfigPath()
{
    if (const char* path = std::getenv("MEADOWLARK_INPUT_CONFIG"); path && *path)
        return path;
    const char* home = std::getenv("HOME");
#ifdef __APPLE__
    return std::string(home ? home : ".") + "/Library/Application Support/Meadowlark/input.toml";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/meadowlark/input.toml";
    return std::string(home ? home : ".") + "/.config/meadowlark/input.toml";
#endif
}

bool loadInputConfig(const std::string& path, InputConfig& config)
{
    config = InputConfig{};
    BindingMap map;

    Loader defaults{ "(built-in input config)" };
    toml::parse_result builtIn = toml::parse(std::string_view(kDefaultConfig), std::string_view("(built-in)"));
    if (!builtIn)
    {
        std::fprintf(stderr, "built-in input config: %s\n", std::string(builtIn.error().description()).c_str());
        return false;
    }
    defaults.readOptions(builtIn.table(), config);
    defaults.readBindings(builtIn.table(), map);

    bool ok = defaults.ok;
    if (!path.empty() && fileExists(path))
    {
        toml::parse_result result = toml::parse_file(path);
        if (!result)
        {
            const toml::parse_error& error = result.error();
            std::fprintf(stderr,
                         "%s:%u: %s (using the default input bindings)\n",
                         path.c_str(),
                         error.source().begin.line,
                         std::string(error.description()).c_str());
            ok = false;
        }
        else
        {
            Loader user{ path };
            if (result.table()["options"]["defaults"].value_or(true) == false)
                map.clear();
            user.readOptions(result.table(), config);
            user.readBindings(result.table(), map);
            std::printf("meadowlark: input config %s%s\n", path.c_str(), user.ok ? "" : " (with errors, see above)");
        }
    }

    for (auto& [action, bindings] : map)
    {
        for (std::vector<InputId>& inputs : bindings)
            config.bindings.push_back(Binding{ std::move(inputs), action });
    }
    return ok;
}
} // namespace input
