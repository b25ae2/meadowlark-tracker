#include "ConfigPaths.h"

#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

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

namespace
{
bool isDirectory(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

#ifndef __APPLE__
// XDG_PICTURES_DIR="$HOME/Pictures" in ~/.config/user-dirs.dirs.
std::string xdgPicturesDir(const std::string& home)
{
    if (const char* path = std::getenv("XDG_PICTURES_DIR"); path && *path)
        return path;
    const char* config = std::getenv("XDG_CONFIG_HOME");
    std::ifstream in((config && *config ? std::string(config) : home + "/.config") + "/user-dirs.dirs");
    std::string line;
    while (std::getline(in, line))
    {
        const std::string key = "XDG_PICTURES_DIR=\"";
        if (line.rfind(key, 0) != 0 || line.back() != '"')
            continue;
        std::string value = line.substr(key.size(), line.size() - key.size() - 1);
        if (value.rfind("$HOME", 0) == 0)
            value = home + value.substr(5);
        return value;
    }
    return home + "/Pictures";
}
#endif
} // namespace

std::string userPicturesPath()
{
    const char* homeEnv = std::getenv("HOME");
    const std::string home = homeEnv ? homeEnv : ".";
#ifdef __APPLE__
    const std::string pictures = home + "/Pictures";
#else
    const std::string pictures = xdgPicturesDir(home);
#endif
    return isDirectory(pictures) ? pictures : home;
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
