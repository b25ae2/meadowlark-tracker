#pragma once

// CMD_LOADIMAGE decoding: JPEG and PNG into the bitmap formats the FT812 produces.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eve
{
struct DecodedImage
{
    uint32_t format = 0; // FT81x bitmap format (RGB565, L8, ARGB4)
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // little-endian, rows packed
};

// Like the chip: JPEG becomes RGB565, or L8 with OPT_MONO; PNG becomes L8 (grey), RGB565
// (colour) or ARGB4 (with alpha). The chip turns palette PNGs into PALETTED formats; here
// they become RGB565 or ARGB4 like colour PNGs.
bool decodeImage(const uint8_t* data, size_t size, bool mono, DecodedImage& out);
} // namespace eve
