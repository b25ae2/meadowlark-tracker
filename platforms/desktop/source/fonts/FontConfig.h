#pragma once

// fonts.toml: system fonts to offer on the tracker's screen. At startup each one is
// rendered into a .ptfont in the cache and shown in the card's /Fonts folder (read-only,
// DesktopFileSystem::addVirtualFile), so Config lists it with the built-in fonts and any
// .ptfont files on the card.

#include "fonts/FontBuilder.h"

#include <string>
#include <vector>

class DesktopFileSystem;

namespace fonts
{
struct FontConfig
{
    std::vector<FontSpec> fonts;
};

const char* defaultFontConfig();

// $MEADOWLARK_FONT_CONFIG, else fonts.toml next to input.toml.
std::string defaultFontConfigPath();

// The file at path if it exists (no file: no fonts). Problems are reported on stderr and
// the entry skipped; returns false when the file is not valid TOML.
bool loadFontConfig(const std::string& path, FontConfig& config);

// Builds every font into cacheDir and adds it to the card as Fonts/<name>.ptfont. Fonts
// that fail are reported and left out. Returns how many were added.
int installFonts(const FontConfig& config, const std::string& cacheDir, DesktopFileSystem& fileSystem);
} // namespace fonts
