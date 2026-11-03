#pragma once

// input.toml: bindings from inputs to actions, plus input options.
//
// The built-in defaults are themselves a TOML document (defaultInputConfig()), which
// `meadowlark --print-input-config` prints as a starting point. A user file only needs the
// actions it changes; with options.defaults = false it replaces the defaults entirely.

#include "InputResolver.h"
#include "SdlInput.h"

#include <string>
#include <vector>

namespace input
{
struct InputConfig
{
    std::vector<Binding> bindings;
    SdlInput::Options options;
    bool captureMouse = false;
};

const char* defaultInputConfig();

// $MEADOWLARK_INPUT_CONFIG, else input.toml in ~/.config/meadowlark ($XDG_CONFIG_HOME
// respected) on Linux and ~/Library/Application Support/Meadowlark on macOS.
std::string defaultInputConfigPath();

// Loads the defaults and then the file at path if it exists. Problems are reported on stderr
// and skipped; returns false when the file exists but is not valid TOML (the defaults are
// then used alone) or a built-in binding is broken.
bool loadInputConfig(const std::string& path, InputConfig& config);
} // namespace input
