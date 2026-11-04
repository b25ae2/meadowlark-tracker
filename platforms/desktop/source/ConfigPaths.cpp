#include "ConfigPaths.h"

#include <cstdlib>

std::string userConfigPath(const char* fileName, const char* envVar)
{
    if (const char* path = std::getenv(envVar); path && *path)
        return path;
    const char* home = std::getenv("HOME");
#ifdef __APPLE__
    return std::string(home ? home : ".") + "/Library/Application Support/Meadowlark/" + fileName;
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/meadowlark/" + fileName;
    return std::string(home ? home : ".") + "/.config/meadowlark/" + fileName;
#endif
}
