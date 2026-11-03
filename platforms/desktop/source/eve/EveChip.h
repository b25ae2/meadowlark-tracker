#pragma once

// Software model of the FT812 (EVE) display controller, for the subset the UI uses.
//
// The host side mirrors what the firmware's SPI driver does: write words into the command
// FIFO, publish them (endCoProList), write and read RAM_G, read REG_CMD_DL. The coprocessor
// runs synchronously when commands are published; CMD_SWAP renders the display list into
// an 800x480 RGBA framebuffer.
//
// Coprocessor widgets are expanded into display-list words like on the chip, except that
// text, numbers and gradients use private opcodes (kOpGlyph, kOpGradient). Controls copy
// their display list into RAM_G and the frame APPENDs it later, so whatever CMD_TEXT emits
// must be self-contained; the private opcodes draw without touching the graphics state.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace eve
{
constexpr uint32_t kRamG = 0x000000;
constexpr uint32_t kRamGSize = 1024 * 1024;
constexpr uint32_t kRamDL = 0x300000;
constexpr uint32_t kRamDLSize = 8 * 1024;
constexpr uint32_t kRamCmd = 0x308000;
constexpr uint32_t kRamCmdSize = 4 * 1024;

constexpr int kScreenWidth = 800;
constexpr int kScreenHeight = 480;

// Display-list opcodes the FT81x leaves unused. kOpGlyph is followed by one word (y << 16 | x),
// kOpGradient by four (x0/y0, rgb0, x1/y1, rgb1).
constexpr uint32_t kOpGlyph = 0x3E;
constexpr uint32_t kOpGradient = 0x3D;

// EVE_TRACE=1 in the environment prints every coprocessor command and swapped display list.
bool traceEnabled();

class Chip
{
  public:
    Chip();

    // --- host interface ------------------------------------------------------------
    void writeCommandWord(uint32_t word);
    void writeCommandBytes(const uint8_t* data, size_t size);
    // Equivalent of updating REG_CMD_WRITE: run everything written so far.
    void executeCommands();
    // Byte offset of the host's write position in RAM_CMD (REG_CMD_WRITE).
    uint32_t commandWriteOffset() const { return cmdWriteOffset; }
    // REG_CMD_DL: bytes the coprocessor has written into RAM_DL since CMD_DLSTART.
    uint32_t displayListOffset() const { return dlOffset; }

    void writeMemory(uint32_t address, const uint8_t* data, size_t size);
    void readMemory(uint32_t address, uint8_t* data, size_t size) const;

    // --- output --------------------------------------------------------------------
    // 0xAARRGGBB, kScreenWidth * kScreenHeight, row-major. Valid after the first swap.
    const uint32_t* frame() const { return framebuffer.data(); }
    uint64_t frameCount() const { return swaps; }

    struct Stats
    {
        uint64_t commandWords = 0;
        uint64_t displayListWords = 0;
        uint64_t unknownCommands = 0;
        uint64_t displayListOverflows = 0;
        uint32_t lastUnknownCommand = 0;
    };
    const Stats& stats() const { return statistics; }

  private:
    // --- memory --------------------------------------------------------------------
    uint8_t readByte(uint32_t address) const;
    uint32_t readWord(uint32_t address) const;
    void writeByte(uint32_t address, uint8_t value);
    void writeWord(uint32_t address, uint32_t value);

    std::vector<uint8_t> ramG;
    std::vector<uint8_t> ramDL;
    std::vector<uint8_t> ramCmd;

    // --- coprocessor (EveCoprocessor.cpp) -------------------------------------------
    // Returns false when the command at the front is incomplete; it waits for more data.
    bool executeOne();
    bool peekWord(size_t index, uint32_t& word) const;
    bool peekString(size_t firstByte, size_t& paddedLength) const;
    void consume(size_t bytes);
    void emit(uint32_t word);
    int resolveFont(int font) const;
    void emitText(int16_t x, int16_t y, int16_t font, uint16_t options, const char* text);
    void emitSetBitmap(uint32_t source, uint32_t format, uint32_t width, uint32_t height);
    void writeResultWord(size_t parameterIndex, uint32_t value);
    bool continueImageLoad();

    std::vector<uint8_t> pending; // bytes written but not yet executed
    size_t pendingStart = 0;
    uint32_t cmdWriteOffset = 0; // host write position in RAM_CMD
    uint32_t cmdReadOffset = 0;  // coprocessor read position in RAM_CMD
    uint32_t dlOffset = 0;

    struct FontInfo
    {
        uint32_t metrics = 0; // RAM_G address of the 148-byte metric block
        uint8_t firstChar = 0;
        bool valid = false;
    };
    std::array<FontInfo, 32> fonts{};
    int numberBase = 10;
    // Coprocessor bitmap matrix, 16.16 fixed point: | a b c | d e f |.
    std::array<int64_t, 6> matrix{};

    struct ImageLoad
    {
        bool active = false;
        uint32_t destination = 0;
        std::vector<uint8_t> data;
    } imageLoad;
    uint32_t lastImageEnd = 0;
    uint32_t lastImageWidth = 0;
    uint32_t lastImageHeight = 0;

    // --- renderer (EveRenderer.cpp) -----------------------------------------------------
    struct BitmapHandle
    {
        uint32_t source = 0;
        uint32_t format = 0;
        uint32_t lineStride = 0;
        uint32_t layoutHeight = 0;
        uint32_t filter = 0;
        uint32_t wrapX = 0;
        uint32_t wrapY = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    void renderDisplayList();
    void snapshot(uint32_t format, uint32_t address, int x, int y, int width, int height);

    // Bitmap handle setup persists across display lists, like the chip's handle registers.
    std::array<BitmapHandle, 32> handles{};
    std::vector<uint32_t> framebuffer;
    uint64_t swaps = 0;

    Stats statistics;
};
} // namespace eve
