#include "FontBuilder.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include "NewDisplay/CompressionRLE.h"
#include "fonts/RobotoMono-Bold_17_L4.h"

#include <algorithm>
#include <cstring>
#include <strings.h>

namespace fonts
{
const char* const kSlotNames[kSlotCount] = { "regular14", "regular17", "bold14", "bold17", "regular12" };
const int kDefaultSizes[kSlotCount] = { 14, 17, 14, 17, 12 };
const bool kSlotIsBold[kSlotCount] = { false, false, true, true, false };

namespace
{
constexpr int kFirstText = 32;
constexpr int kLastChar = 126;
constexpr uint32_t kMetricBytes = 148;
constexpr uint32_t kFormatL4 = 2;

// The bold 17 slot also holds the transport icons the song timer draws (songTimer.cpp),
// two characters each, from 0x18. They are taken from the built-in font.
constexpr int kIconSlot = 3;
constexpr int kFirstIcon = 0x18;

struct IconSource
{
    std::vector<uint8_t> font; // decompressed built-in Roboto Mono Bold 17
    uint32_t stride = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int firstChar = 0;

    IconSource()
    {
        font.assign(kMaxFontBytes, 0);
        decompress(Roboto_Mono_Bold_17_L4_compressed,
                   sizeof(Roboto_Mono_Bold_17_L4_compressed) / sizeof(Roboto_Mono_Bold_17_L4_compressed[0]),
                   font.data());
        std::memcpy(&stride, &font[132], 4);
        std::memcpy(&width, &font[136], 4);
        std::memcpy(&height, &font[140], 4);
        while (firstChar < 127 && font[static_cast<size_t>(firstChar)] == 0)
            firstChar++;
    }

    int level(int ch, int x, int y) const
    {
        const uint8_t* cell = &font[kMetricBytes + static_cast<size_t>((ch - firstChar) * stride * height)];
        return (cell[y * stride + x / 2] >> (x % 2 ? 0 : 4)) & 15;
    }
};

struct Library
{
    FT_Library handle = nullptr;
    Library() { FT_Init_FreeType(&handle); }
    ~Library()
    {
        if (handle)
            FT_Done_FreeType(handle);
    }
};

class Face
{
  public:
    ~Face()
    {
        if (face)
            FT_Done_Face(face);
    }
    FT_Face get() const { return face; }

    // Finds the face (or named instance) that FaceSpec asks for.
    bool open(FT_Library library, const FaceSpec& spec, std::string& error)
    {
        FT_Face probe = nullptr;
        if (FT_New_Face(library, spec.file.c_str(), -1, &probe) != 0)
        {
            error = "cannot open " + spec.file;
            return false;
        }
        const FT_Long faces = probe->num_faces;
        FT_Done_Face(probe);

        std::string available;
        for (FT_Long index = 0; index < faces; index++)
        {
            FT_Face candidate = nullptr;
            if (FT_New_Face(library, spec.file.c_str(), index, &candidate) != 0)
                continue;
            const FT_Long instances = candidate->style_flags >> 16;
            for (FT_Long instance = 0; instance <= instances; instance++)
            {
                if (instance > 0)
                {
                    FT_Done_Face(candidate);
                    candidate = nullptr;
                    if (FT_New_Face(library, spec.file.c_str(), (instance << 16) | index, &candidate) != 0)
                        break;
                }
                if (matches(candidate, spec))
                {
                    face = candidate;
                    return true;
                }
                available += std::string(available.empty() ? "" : ", ") + candidate->family_name + " " +
                             candidate->style_name;
            }
            if (candidate)
                FT_Done_Face(candidate);
        }
        error = spec.file + " has no face " + spec.family + (spec.family.empty() ? "" : " ") + spec.style +
                " (it has: " + available + ")";
        return false;
    }

  private:
    static bool matches(FT_Face face, const FaceSpec& spec)
    {
        auto same = [](const char* a, const std::string& b) { return b.empty() || (a && strcasecmp(a, b.c_str()) == 0); };
        return same(face->family_name, spec.family) && same(face->style_name, spec.style);
    }

    FT_Face face = nullptr;
};

struct Glyph
{
    int advance = 0;
    int left = 0;
    int top = 0;
    int width = 0;
    int rows = 0;
    std::vector<uint8_t> coverage; // width * rows, 0..255
};

bool renderGlyph(FT_Face face, int ch, bool embolden, Glyph& glyph)
{
    if (FT_Load_Char(face, static_cast<FT_ULong>(ch), FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT) != 0)
        return false;
    FT_GlyphSlot slot = face->glyph;
    if (embolden)
        FT_GlyphSlot_Embolden(slot);
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0)
        return false;

    const FT_Bitmap& bitmap = slot->bitmap;
    glyph.advance = static_cast<int>((slot->advance.x + 32) >> 6);
    glyph.left = slot->bitmap_left;
    glyph.top = slot->bitmap_top;
    glyph.width = static_cast<int>(bitmap.width);
    glyph.rows = static_cast<int>(bitmap.rows);
    glyph.coverage.assign(static_cast<size_t>(glyph.width * glyph.rows), 0);
    for (int y = 0; y < glyph.rows; y++)
    {
        const uint8_t* row = bitmap.buffer + y * bitmap.pitch;
        for (int x = 0; x < glyph.width; x++)
        {
            uint8_t value = 0;
            if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                value = (row[x / 8] >> (7 - x % 8)) & 1 ? 255 : 0;
            else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY)
                value = row[x];
            glyph.coverage[static_cast<size_t>(y * glyph.width + x)] = value;
        }
    }
    return true;
}

void putWord(std::vector<uint8_t>& out, size_t at, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        out[at + static_cast<size_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
}

// One legacy font: metric block plus cells from firstChar to '~'. address is where core
// will put it in RAM_G.
bool buildSlot(FT_Face face, int pixelSize, bool embolden, bool withIcons, uint32_t address,
               std::vector<uint8_t>& out, SlotInfo& info, std::string& error)
{
    static const IconSource icons;
    const int firstChar = withIcons ? kFirstIcon : kFirstText;
    const int charCount = kLastChar - firstChar + 1;

    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize)) != 0)
    {
        error = "cannot set size " + std::to_string(pixelSize);
        return false;
    }

    std::vector<Glyph> glyphs(static_cast<size_t>(charCount));
    int top = 0;
    int bottom = 0;
    int width = withIcons ? static_cast<int>(icons.width) : 1; // icon pairs need their width
    for (int i = 0; i < charCount; i++)
    {
        Glyph& glyph = glyphs[static_cast<size_t>(i)];
        const int ch = firstChar + i;
        if (ch < kFirstText)
        {
            glyph.advance = icons.font[static_cast<size_t>(ch)];
            continue;
        }
        if (!renderGlyph(face, ch, embolden, glyph))
        {
            error = "cannot render character " + std::to_string(ch);
            return false;
        }
        // Every character needs a width: core finds the first one by it and counts cells.
        glyph.advance = std::clamp(glyph.advance, 1, 255);
        width = std::max({ width, glyph.advance, std::max(glyph.left, 0) + glyph.width });
        if (glyph.rows > 0)
        {
            top = std::max(top, glyph.top);
            bottom = std::min(bottom, glyph.top - glyph.rows);
        }
    }
    const int height = std::max(1, top - bottom);
    const int stride = (width + 1) / 2;

    const uint32_t bytes = kMetricBytes + static_cast<uint32_t>(charCount * stride * height);
    if (bytes > kMaxFontBytes)
    {
        error = "size " + std::to_string(pixelSize) + " is too big for the tracker (" + std::to_string(bytes) +
                " bytes, at most " + std::to_string(kMaxFontBytes) + ")";
        return false;
    }

    out.assign(bytes, 0);
    for (int i = 0; i < charCount; i++)
        out[static_cast<size_t>(firstChar + i)] = static_cast<uint8_t>(glyphs[static_cast<size_t>(i)].advance);
    putWord(out, 128, kFormatL4);
    putWord(out, 132, static_cast<uint32_t>(stride));
    putWord(out, 136, static_cast<uint32_t>(width));
    putWord(out, 140, static_cast<uint32_t>(height));
    putWord(out, 144, address + kMetricBytes); // the display driver sets it again on upload

    // L4: two pixels a byte, the left one in the high nibble.
    for (int i = 0; i < charCount; i++)
    {
        const Glyph& glyph = glyphs[static_cast<size_t>(i)];
        uint8_t* cell = &out[kMetricBytes + static_cast<size_t>(i * stride * height)];
        if (firstChar + i < kFirstText)
        {
            // An icon: centred vertically, cropped if the cell is shorter.
            const int shift = (static_cast<int>(icons.height) - height) / 2;
            for (int y = 0; y < height; y++)
            {
                const int sy = y + shift;
                if (sy < 0 || sy >= static_cast<int>(icons.height))
                    continue;
                for (int x = 0; x < static_cast<int>(icons.width); x++)
                {
                    const uint8_t level = static_cast<uint8_t>(icons.level(firstChar + i, x, sy));
                    cell[y * stride + x / 2] |= x % 2 ? level : static_cast<uint8_t>(level << 4);
                }
            }
            continue;
        }
        for (int y = 0; y < glyph.rows; y++)
        {
            const int cy = top - glyph.top + y;
            if (cy < 0 || cy >= height)
                continue;
            for (int x = 0; x < glyph.width; x++)
            {
                const int cx = glyph.left + x;
                if (cx < 0 || cx >= width)
                    continue;
                const uint8_t level =
                  static_cast<uint8_t>((glyph.coverage[static_cast<size_t>(y * glyph.width + x)] * 15 + 127) / 255);
                cell[cy * stride + cx / 2] |= cx % 2 ? level : static_cast<uint8_t>(level << 4);
            }
        }
    }

    info.pixelSize = pixelSize;
    info.cellWidth = width;
    info.cellHeight = height;
    info.bytes = bytes;
    return true;
}
} // namespace

bool buildPtFont(const FontSpec& spec, std::vector<uint8_t>& out, std::string& error, SlotInfo info[kSlotCount])
{
    Library library;
    if (!library.handle)
    {
        error = "FreeType did not start";
        return false;
    }
    Face regular;
    if (!regular.open(library.handle, spec.regular, error))
        return false;
    Face bold;
    const bool synthetic = spec.bold.file.empty();
    if (!synthetic && !bold.open(library.handle, spec.bold, error))
        return false;

    // Header (16 bytes) and directory (12 bytes a font), then the fonts.
    constexpr size_t kHeaderBytes = 16;
    constexpr size_t kEntryBytes = 12;
    out.assign(kHeaderBytes + kEntryBytes * kSlotCount, 0);
    std::memcpy(out.data(), "PTFN", 4);
    out[4] = 1; // version
    out[6] = kSlotCount;

    uint32_t address = 0; // RAM_G, laid out as loadCustomFontFromSD does
    for (int slot = 0; slot < kSlotCount; slot++)
    {
        const bool isBold = kSlotIsBold[slot];
        FT_Face face = isBold && !synthetic ? bold.get() : regular.get();
        std::vector<uint8_t> data;
        SlotInfo slotInfo;
        if (!buildSlot(face, spec.sizes[slot], isBold && synthetic, slot == kIconSlot, address, data, slotInfo, error))
        {
            error = std::string(kSlotNames[slot]) + ": " + error;
            return false;
        }
        if (info)
            info[slot] = slotInfo;

        const size_t entry = kHeaderBytes + kEntryBytes * static_cast<size_t>(slot);
        putWord(out, entry, static_cast<uint32_t>(out.size()));
        putWord(out, entry + 4, static_cast<uint32_t>(data.size()));
        out[entry + 8] = static_cast<uint8_t>(slot + 1); // handle
        out[entry + 10] = kFormatL4;
        out.insert(out.end(), data.begin(), data.end());

        address += static_cast<uint32_t>(data.size());
        address = (address + 3) & ~3u;
    }
    if (address > kMaxTotalFontBytes)
    {
        error = "the five sizes take " + std::to_string(address) + " bytes, the tracker has room for " +
                std::to_string(kMaxTotalFontBytes);
        return false;
    }
    return true;
}
} // namespace fonts
