#pragma once

// Where the user's configuration files live.

#include <string>

// $<envVar> if set, else fileName in ~/.config/meadowlark ($XDG_CONFIG_HOME respected) on
// Linux and ~/Library/Application Support/Meadowlark on macOS.
std::string userConfigPath(const char* fileName, const char* envVar);

// Directory for files Meadowlark can regenerate: $XDG_CACHE_HOME/meadowlark/<name> (usually
// ~/.cache/meadowlark/<name>) on Linux, ~/Library/Caches/Meadowlark/<name> on macOS.
std::string userCachePath(const char* name);

// Where screenshots go: ~/Pictures on macOS; on Linux the XDG pictures directory
// ($XDG_PICTURES_DIR or user-dirs.dirs), else ~/Pictures; the home directory if that is
// missing.
std::string userPicturesPath();

// "~/x" becomes $HOME/x; other paths are returned as they are.
std::string expandHome(const std::string& path);
