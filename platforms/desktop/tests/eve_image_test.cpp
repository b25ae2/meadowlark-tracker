// CMD_LOADIMAGE in the software FT812: images streamed into the command FIFO the way core
// does it (display.cpp: the command, then the file in 512-byte chunks padded to words),
// decoded into RAM_G, reported by CMD_GETPROPS and drawn as a bitmap.

#include "eve/EveChip.h"
#include "eve_image_data.h"

#include "FT81x.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
int failures = 0;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

constexpr uint32_t kAddress = 670000; // where fProjEdit loads the project cover
constexpr uint32_t kWidth = 32;
constexpr uint32_t kHeight = 16;

void streamImage(eve::Chip& chip, uint32_t options, const uint8_t* data, size_t size)
{
    chip.writeCommandWord(CMD_LOADIMAGE);
    chip.writeCommandWord(kAddress);
    chip.writeCommandWord(options);
    chip.executeCommands();

    constexpr size_t kChunk = 512;
    for (size_t offset = 0; offset < size; offset += kChunk)
    {
        size_t length = size - offset < kChunk ? size - offset : kChunk;
        std::vector<uint8_t> chunk(data + offset, data + offset + length);
        chunk.resize((length + 3) & ~size_t(3), 0);
        chip.writeCommandBytes(chunk.data(), chunk.size());
        chip.executeCommands();
    }
}

struct Props
{
    uint32_t end = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// CMD_GETPROPS writes its results over its three parameter words, like on the chip.
Props getProps(eve::Chip& chip)
{
    chip.writeCommandWord(CMD_GETPROPS);
    chip.writeCommandWord(0);
    chip.writeCommandWord(0);
    chip.writeCommandWord(0);
    chip.executeCommands();
    const uint32_t offset = chip.commandWriteOffset();
    uint32_t words[3];
    for (int i = 0; i < 3; i++)
    {
        uint8_t bytes[4];
        chip.readMemory(eve::kRamCmd + ((offset - 12 + 4 * i) & (eve::kRamCmdSize - 1)), bytes, 4);
        words[i] = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | static_cast<uint32_t>(bytes[3]) << 24;
    }
    return { words[0], words[1], words[2] };
}

uint16_t read16(const eve::Chip& chip, uint32_t index)
{
    uint8_t bytes[2];
    chip.readMemory(kAddress + index * 2, bytes, 2);
    return static_cast<uint16_t>(bytes[0] | bytes[1] << 8);
}

// Draws the loaded bitmap at (0, 0) on black and returns the frame.
const uint32_t* drawBitmap(eve::Chip& chip, uint32_t format)
{
    chip.writeCommandWord(CMD_DLSTART);
    chip.writeCommandWord(CLEAR_COLOR_RGB(0, 0, 0));
    chip.writeCommandWord(CLEAR(1, 1, 1));
    chip.writeCommandWord(CMD_SETBITMAP);
    chip.writeCommandWord(kAddress);
    chip.writeCommandWord(format | kWidth << 16);
    chip.writeCommandWord(kHeight);
    chip.writeCommandWord(BEGIN(BITMAPS));
    chip.writeCommandWord(VERTEX2II(0, 0, 0, 0));
    chip.writeCommandWord(END());
    chip.writeCommandWord(0); // DISPLAY
    chip.writeCommandWord(CMD_SWAP);
    chip.executeCommands();
    return chip.frame();
}

bool near(uint32_t argb, uint8_t r, uint8_t g, uint8_t b, int tolerance)
{
    auto close = [tolerance](int a, int e) { return std::abs(a - e) <= tolerance; };
    return close((argb >> 16) & 0xFF, r) && close((argb >> 8) & 0xFF, g) && close(argb & 0xFF, b);
}

// Red left half, blue right half, green bottom-right block.
void checkColourFrame(const uint32_t* frame, int tolerance)
{
    CHECK(near(frame[2 * eve::kScreenWidth + 2], 255, 0, 0, tolerance));
    CHECK(near(frame[2 * eve::kScreenWidth + 29], 0, 0, 255, tolerance));
    CHECK(near(frame[12 * eve::kScreenWidth + 29], 0, 255, 0, tolerance));
    CHECK(near(frame[12 * eve::kScreenWidth + 2], 255, 0, 0, tolerance));
    CHECK(near(frame[40 * eve::kScreenWidth + 40], 0, 0, 0, 0)); // outside the bitmap
}

void testPng()
{
    eve::Chip chip;
    streamImage(chip, OPT_NODL, kRgbPng, sizeof(kRgbPng));
    Props props = getProps(chip);
    CHECK(props.width == kWidth && props.height == kHeight);
    CHECK(props.end == kAddress + kWidth * kHeight * 2);
    CHECK(read16(chip, 0) == 0xF800);                    // red
    CHECK(read16(chip, 31) == 0x001F);                   // blue
    CHECK(read16(chip, 15 * kWidth + 31) == 0x07E0);     // green
    checkColourFrame(drawBitmap(chip, RGB565), 0);
}

void testPngAlpha()
{
    eve::Chip chip;
    streamImage(chip, OPT_NODL, kRgbaPng, sizeof(kRgbaPng));
    Props props = getProps(chip);
    CHECK(props.width == kWidth && props.end == kAddress + kWidth * kHeight * 2);
    CHECK(read16(chip, 0) == 0xFF00);  // opaque red
    CHECK(read16(chip, 31) == 0x000F); // transparent blue
    const uint32_t* frame = drawBitmap(chip, ARGB4);
    CHECK(near(frame[2 * eve::kScreenWidth + 2], 255, 0, 0, 0));
    CHECK(near(frame[2 * eve::kScreenWidth + 29], 0, 0, 0, 0)); // blended away
}

void testPngGrey()
{
    eve::Chip chip;
    streamImage(chip, OPT_NODL, kGreyPng, sizeof(kGreyPng));
    Props props = getProps(chip);
    CHECK(props.width == kWidth && props.end == kAddress + kWidth * kHeight);
    uint8_t pixel = 0;
    chip.readMemory(kAddress + 10, &pixel, 1);
    CHECK(pixel == 80);
}

void testJpeg()
{
    eve::Chip chip;
    streamImage(chip, OPT_NODL, kRgbJpeg, sizeof(kRgbJpeg));
    Props props = getProps(chip);
    CHECK(props.width == kWidth && props.height == kHeight);
    CHECK(props.end == kAddress + kWidth * kHeight * 2);
    checkColourFrame(drawBitmap(chip, RGB565), 24);

    // OPT_MONO gives L8.
    eve::Chip mono;
    streamImage(mono, OPT_NODL | OPT_MONO, kRgbJpeg, sizeof(kRgbJpeg));
    props = getProps(mono);
    CHECK(props.end == kAddress + kWidth * kHeight);
}

void testDisplayListOutput()
{
    // Without OPT_NODL the coprocessor also emits the bitmap setup, so a bare BEGIN/VERTEX
    // after it draws the image.
    eve::Chip chip;
    chip.writeCommandWord(CMD_DLSTART);
    chip.writeCommandWord(CLEAR_COLOR_RGB(0, 0, 0));
    chip.writeCommandWord(CLEAR(1, 1, 1));
    chip.executeCommands();
    streamImage(chip, 0, kRgbPng, sizeof(kRgbPng));
    chip.writeCommandWord(BEGIN(BITMAPS));
    chip.writeCommandWord(VERTEX2II(0, 0, 0, 0));
    chip.writeCommandWord(END());
    chip.writeCommandWord(0);
    chip.writeCommandWord(CMD_SWAP);
    chip.executeCommands();
    checkColourFrame(chip.frame(), 0);
}

} // namespace

int main()
{
    testPng();
    testPngAlpha();
    testPngGrey();
    testJpeg();
    testDisplayListOutput();
    if (failures)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all image checks passed\n");
    return 0;
}
