#include "PngWriter.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (uint32_t n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
            {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; i++)
    {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
    put32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(&out[start], out.size() - start));
}
} // namespace

bool writePng(const char* path, const uint32_t* pixels, int width, int height)
{
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + 3 * width));
    for (int y = 0; y < height; y++)
    {
        raw.push_back(0); // filter: none
        for (int x = 0; x < width; x++)
        {
            const uint32_t p = pixels[static_cast<size_t>(y) * width + x];
            raw.push_back(static_cast<uint8_t>(p >> 16));
            raw.push_back(static_cast<uint8_t>(p >> 8));
            raw.push_back(static_cast<uint8_t>(p));
        }
    }

    // zlib stream of stored deflate blocks, then Adler-32.
    std::vector<uint8_t> z = { 0x78, 0x01 };
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < raw.size();)
    {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        for (size_t i = 0; i < n; i++)
        {
            const uint8_t v = raw[pos + i];
            z.push_back(v);
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
        pos += n;
    }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<uint8_t> header;
    put32(header, static_cast<uint32_t>(width));
    put32(header, static_cast<uint32_t>(height));
    header.insert(header.end(), { 8, 2, 0, 0, 0 }); // 8-bit, RGB, deflate, no filter, no interlace
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});

    FILE* f = std::fopen(path, "wb");
    if (!f)
    {
        return false;
    }
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}
