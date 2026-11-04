#include "FontConfig.h"

#include "ConfigPaths.h"
#include "DesktopFileSystem.h"

#include <toml++/toml.hpp>

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>

namespace fonts
{
namespace
{
constexpr char kDefaultConfig[] = R"toml(# Fonts for the tracker's screen.
#
# Each [[font]] is rendered at startup from a TTF, OTF or TTC file into the tracker's font
# format and appears in Config > General > Display font under its name, after the built-in
# fonts. (Font files made for the tracker, .ptfont, can also go in the card's /Fonts folder.)
#
# The UI uses five sizes: regular14, regular17, bold14, bold17 and regular12, named after
# the pixel sizes the built-in fonts use. Fonts with taller or wider letters than those
# may not fit everywhere; smaller sizes help.
#
# [[font]]
# name = "DejaVu Mono"     # up to 24 characters
# regular = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
# bold = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"   # optional; without
#                          # it the regular face is made bolder
#
# A face can also be a table, to choose one in a collection (.ttc) or a variable font by
# its family and style (an error lists the ones a file has):
#
# [[font]]
# name = "Iosevka"
# regular = { file = "~/.local/share/fonts/iosevka/Iosevka-Regular.ttc", family = "Iosevka Term" }
# bold = { file = "~/.local/share/fonts/iosevka/Iosevka-Bold.ttc", family = "Iosevka Term", style = "Bold" }
# sizes = { regular14 = 13, bold14 = 13 }   # pixel sizes, only the ones to change
)toml";

void warn(const toml::node& node, const std::string& message)
{
    const toml::source_region& where = node.source();
    std::fprintf(stderr,
                 "%s:%u: %s\n",
                 where.path ? where.path->c_str() : "fonts.toml",
                 static_cast<unsigned>(where.begin.line),
                 message.c_str());
}

bool readFace(const toml::node& node, FaceSpec& face)
{
    if (std::optional<std::string> file = node.value<std::string>())
    {
        face.file = expandHome(*file);
        return true;
    }
    const toml::table* table = node.as_table();
    if (!table)
    {
        warn(node, "a face is a file name or { file = ..., family = ..., style = ... }");
        return false;
    }
    std::optional<std::string> file = (*table)["file"].value<std::string>();
    if (!file)
    {
        warn(node, "the face has no file");
        return false;
    }
    face.file = expandHome(*file);
    face.family = (*table)["family"].value_or(std::string());
    face.style = (*table)["style"].value_or(std::string());
    return true;
}

bool readFont(const toml::table& table, FontSpec& font)
{
    std::optional<std::string> name = table["name"].value<std::string>();
    if (!name || name->empty())
    {
        warn(table, "a [[font]] needs a name");
        return false;
    }
    // A file name in /Fonts, which the tracker lists up to 31 characters long.
    font.name.clear();
    for (char c : *name)
        font.name += (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32) ? '_' : c;
    if (font.name.size() > 24)
        font.name.resize(24);

    const toml::node* regular = table.get("regular");
    if (!regular)
    {
        warn(table, "font \"" + *name + "\" needs a regular face");
        return false;
    }
    if (!readFace(*regular, font.regular))
        return false;
    if (const toml::node* bold = table.get("bold"); bold && !readFace(*bold, font.bold))
        return false;

    for (int slot = 0; slot < kSlotCount; slot++)
        font.sizes[slot] = kDefaultSizes[slot];
    if (const toml::node* sizes = table.get("sizes"))
    {
        const toml::table* list = sizes->as_table();
        if (!list)
        {
            warn(*sizes, "sizes is a table: { regular14 = 14, ... }");
            return false;
        }
        for (auto&& [key, value] : *list)
        {
            int slot = 0;
            while (slot < kSlotCount && key.str() != kSlotNames[slot])
                slot++;
            std::optional<int64_t> size = value.value<int64_t>();
            if (slot == kSlotCount)
                warn(value, "unknown size \"" + std::string(key.str()) + "\"");
            else if (!size || *size < 6 || *size > 40)
                warn(value, std::string(kSlotNames[slot]) + " must be a pixel size from 6 to 40");
            else
                font.sizes[slot] = static_cast<int>(*size);
        }
    }
    return true;
}

bool fileExists(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool makeDirectories(const std::string& path)
{
    for (size_t slash = path.find('/', 1); ; slash = path.find('/', slash + 1))
    {
        const std::string part = path.substr(0, slash);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST)
            return false;
        if (slash == std::string::npos)
            return true;
    }
}
} // namespace

const char* defaultFontConfig()
{
    return kDefaultConfig;
}

std::string defaultFontConfigPath()
{
    return userConfigPath("fonts.toml", "MEADOWLARK_FONT_CONFIG");
}

bool loadFontConfig(const std::string& path, FontConfig& config)
{
    config = FontConfig{};
    if (path.empty() || !fileExists(path))
        return true;
    toml::parse_result result = toml::parse_file(path);
    if (!result)
    {
        const toml::parse_error& error = result.error();
        std::fprintf(stderr,
                     "%s:%u: %s (no fonts added)\n",
                     path.c_str(),
                     static_cast<unsigned>(error.source().begin.line),
                     std::string(error.description()).c_str());
        return false;
    }
    for (auto&& [key, value] : result.table())
    {
        if (key.str() != "font")
            warn(value, "unknown setting \"" + std::string(key.str()) + "\"");
    }
    if (const toml::node* fonts = result.table().get("font"))
    {
        const toml::array* list = fonts->as_array();
        if (!list)
        {
            warn(*fonts, "fonts are [[font]] tables");
            return true;
        }
        for (const toml::node& entry : *list)
        {
            FontSpec font;
            if (const toml::table* table = entry.as_table(); table && readFont(*table, font))
                config.fonts.push_back(font);
        }
    }
    return true;
}

int installFonts(const FontConfig& config, const std::string& cacheDir, DesktopFileSystem& fileSystem)
{
    if (config.fonts.empty())
        return 0;
    if (!makeDirectories(cacheDir))
    {
        std::fprintf(stderr, "meadowlark: cannot create %s for fonts\n", cacheDir.c_str());
        return 0;
    }

    int added = 0;
    for (const FontSpec& font : config.fonts)
    {
        std::vector<uint8_t> data;
        std::string error;
        SlotInfo info[kSlotCount];
        if (!buildPtFont(font, data, error, info))
        {
            std::fprintf(stderr, "meadowlark: font \"%s\": %s\n", font.name.c_str(), error.c_str());
            continue;
        }
        const std::string fileName = font.name + ".ptfont";
        const std::string hostPath = cacheDir + "/" + fileName;
        FILE* file = std::fopen(hostPath.c_str(), "wb");
        bool written = file && std::fwrite(data.data(), 1, data.size(), file) == data.size();
        if (file)
            written = std::fclose(file) == 0 && written;
        if (!written)
        {
            std::fprintf(stderr, "meadowlark: cannot write %s\n", hostPath.c_str());
            continue;
        }
        fileSystem.addVirtualFile("Fonts/" + fileName, hostPath);

        std::string cells;
        for (int slot = 0; slot < kSlotCount; slot++)
            cells += std::string(slot ? ", " : "") + std::to_string(info[slot].cellWidth) + "x" +
                     std::to_string(info[slot].cellHeight);
        std::printf("meadowlark: font %s (cells %s)\n", font.name.c_str(), cells.c_str());
        added++;
    }
    return added;
}
} // namespace fonts
