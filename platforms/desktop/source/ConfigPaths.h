#pragma once

// Where the user's configuration files live.

#include <string>

// $<envVar> if set, else fileName in ~/.config/meadowlark ($XDG_CONFIG_HOME respected) on
// Linux and ~/Library/Application Support/Meadowlark on macOS.
std::string userConfigPath(const char* fileName, const char* envVar);
