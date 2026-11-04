#include "EveImage.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
#include "stb/stb_image.h"

// After stb: its macros (L8, BORDER, ...) would clash with names inside it.
#include "FT81x.h"

#include <climits>

namespace eve
{
namespace
{
bool isPng(const uint8_t* data, size_t size)
{
    return size >= 4 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G';
}

void put16(std::vector<uint8_t>& out, size_t index, uint16_t value)
{
    out[index * 2] = static_cast<uint8_t>(value);
    out[index * 2 + 1] = static_cast<uint8_t>(value >> 8);
}
} // namespace

bool decodeImage(const uint8_t* data, size_t size, bool mono, DecodedImage& out)
{
    if (size > static_cast<size_t>(INT_MAX))
        return false;

    int width = 0;
    int height = 0;
    int fileChannels = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(size), &width, &height, &fileChannels))
        return false;

    // Pick the chip's output format from the file, then let stb convert to what that needs.
    int channels = 3;
    if (isPng(data, size))
    {
        if (fileChannels == 1)
            channels = 1;
        else if (fileChannels == 2 || fileChannels == 4)
            channels = 4;
    }
    else if (mono)
    {
        channels = 1;
    }

    int decodedChannels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &decodedChannels, channels);
    if (!pixels)
        return false;

    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    if (channels == 1)
    {
        out.format = L8;
        out.pixels.assign(pixels, pixels + count);
    }
    else if (channels == 3)
    {
        out.format = RGB565;
        out.pixels.resize(count * 2);
        for (size_t i = 0; i < count; i++)
        {
            const stbi_uc* p = pixels + i * 3;
            put16(out.pixels, i, static_cast<uint16_t>((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3));
        }
    }
    else
    {
        out.format = ARGB4;
        out.pixels.resize(count * 2);
        for (size_t i = 0; i < count; i++)
        {
            const stbi_uc* p = pixels + i * 4;
            put16(out.pixels, i, static_cast<uint16_t>((p[3] >> 4) << 12 | (p[0] >> 4) << 8 | (p[1] >> 4) << 4 | p[2] >> 4));
        }
    }
    stbi_image_free(pixels);
    return true;
}
} // namespace eve
