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

std::string userCachePath(const char* name)
{
    const char* home = std::getenv("HOME");
#ifdef __APPLE__
    return std::string(home ? home : ".") + "/Library/Caches/Meadowlark/" + name;
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return std::string(xdg) + "/meadowlark/" + name;
    return std::string(home ? home : ".") + "/.cache/meadowlark/" + name;
#endif
}

std::string expandHome(const std::string& path)
{
    if (path == "~" || path.rfind("~/", 0) == 0)
    {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "") + path.substr(1);
    }
    return path;
}
