#pragma once

// Renders TTF/OTF/TTC fonts into the firmware's .ptfont format: the five FT812 legacy
// fonts the UI uses (slots regular14, regular17, bold14, bold17, regular12), each a
// 148-byte metric block (advance widths for ASCII, format, stride, cell size, pointer)
// followed by one 4-bit (L4) cell per character from ' ' to '~'. bold17 also carries the
// firmware's transport icons (characters 0x18 to 0x1F), copied from its built-in font.
//
// Cells are as tall as the glyphs reach above and below the baseline, as with the EVE
// asset converter that made the built-in fonts; text is drawn from the cell's top.

#include <cstdint>
#include <string>
#include <vector>

namespace fonts
{
constexpr int kSlotCount = 5;
// Slot names in the order core keeps them (display.cpp: fontsData1).
extern const char* const kSlotNames[kSlotCount];
extern const int kDefaultSizes[kSlotCount];
// Which slots use the bold face.
extern const bool kSlotIsBold[kSlotCount];

// One face of a font file. family and style pick a face in a collection (.ttc) or a named
// instance of a variable font; empty takes the first.
struct FaceSpec
{
    std::string file;
    std::string family;
    std::string style;
};

struct FontSpec
{
    std::string name;
    FaceSpec regular;
    FaceSpec bold;      // file empty: the regular face, emboldened
    int sizes[kSlotCount] = { 14, 17, 14, 17, 12 };
};

// What one slot came out as, for reports.
struct SlotInfo
{
    int pixelSize = 0;
    int cellWidth = 0;
    int cellHeight = 0;
    uint32_t bytes = 0;
};

// The .ptfont file's bytes. On failure returns false and says why in error.
bool buildPtFont(const FontSpec& spec, std::vector<uint8_t>& out, std::string& error, SlotInfo info[kSlotCount] = nullptr);

// Sizes the firmware accepts: each font at most this many bytes (its load buffer), all of
// them together below its first other RAM_G user.
constexpr uint32_t kMaxFontBytes = 22396;
constexpr uint32_t kMaxTotalFontBytes = 150000;
} // namespace fonts
