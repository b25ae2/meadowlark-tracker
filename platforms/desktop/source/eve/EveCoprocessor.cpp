// FT812 coprocessor: memory map, command FIFO parsing and widget expansion.

#include "EveChip.h"

#include "FT81x.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace eve
{
namespace
{
constexpr uint32_t kAddressMask = 0x3FFFFF;

// Display-list opcodes used when expanding coprocessor commands.
constexpr uint32_t dlBitmapHandle(uint32_t h) { return (5u << 24) | (h & 31); }
constexpr uint32_t dlBitmapSource(uint32_t a) { return (1u << 24) | (a & kAddressMask); }
constexpr uint32_t dlBitmapLayout(uint32_t f, uint32_t stride, uint32_t h)
{
    return (7u << 24) | ((f & 31) << 19) | ((stride & 1023) << 9) | (h & 511);
}
constexpr uint32_t dlBitmapLayoutH(uint32_t stride, uint32_t h)
{
    return (40u << 24) | (((stride >> 10) & 3) << 2) | ((h >> 9) & 3);
}
constexpr uint32_t dlBitmapSize(uint32_t filter, uint32_t wx, uint32_t wy, uint32_t w, uint32_t h)
{
    return (8u << 24) | ((filter & 1) << 20) | ((wx & 1) << 19) | ((wy & 1) << 18) |
           ((w & 511) << 9) | (h & 511);
}
constexpr uint32_t dlBitmapSizeH(uint32_t w, uint32_t h)
{
    return (41u << 24) | (((w >> 9) & 3) << 2) | ((h >> 9) & 3);
}

// Bits per pixel of the formats CMD_SETBITMAP can be given.
uint32_t bitsPerPixel(uint32_t format)
{
    switch (format)
    {
        case L1: return 1;
        case L2: return 2;
        case L4: return 4;
        case L8:
        case RGB332:
        case ARGB2:
        case PALETTED8: return 8;
        default: return 16;
    }
}

uint16_t readBigEndian16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t readBigEndian32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// Length of a complete PNG or JPEG at the start of data, or 0 if it is not complete yet.
// Also reports the image size from its header.
size_t imageLength(const std::vector<uint8_t>& data, uint32_t& width, uint32_t& height)
{
    static const uint8_t kPngSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    const size_t size = data.size();

    if (size >= 8 && std::memcmp(data.data(), kPngSignature, 8) == 0)
    {
        size_t pos = 8;
        while (pos + 12 <= size)
        {
            const uint32_t length = readBigEndian32(&data[pos]);
            const uint8_t* type = &data[pos + 4];
            if (std::memcmp(type, "IHDR", 4) == 0 && pos + 16 <= size)
            {
                width = readBigEndian32(&data[pos + 8]);
                height = readBigEndian32(&data[pos + 12]);
            }
            const size_t next = pos + 12 + length;
            if (std::memcmp(type, "IEND", 4) == 0)
            {
                return next <= size ? next : 0;
            }
            pos = next;
        }
        return 0;
    }

    if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8)
    {
        // Walk marker segments up to start-of-scan, then scan the entropy-coded data, where
        // 0xFF is always followed by 0x00 or a restart marker until end-of-image.
        size_t pos = 2;
        while (pos + 4 <= size)
        {
            if (data[pos] != 0xFF)
            {
                return 0; // corrupt; keep waiting rather than guessing
            }
            const uint8_t marker = data[pos + 1];
            if (marker == 0xD9)
            {
                return pos + 2;
            }
            const uint16_t length = readBigEndian16(&data[pos + 2]);
            const bool isFrameHeader = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                                       marker != 0xC8 && marker != 0xCC;
            if (isFrameHeader && pos + 9 <= size)
            {
                height = readBigEndian16(&data[pos + 5]);
                width = readBigEndian16(&data[pos + 7]);
            }
            pos += 2 + length;
            if (marker == 0xDA)
            {
                for (; pos + 1 < size; pos++)
                {
                    if (data[pos] == 0xFF && data[pos + 1] != 0x00 &&
                        !(data[pos + 1] >= 0xD0 && data[pos + 1] <= 0xD7))
                    {
                        break;
                    }
                }
            }
        }
        return 0;
    }

    return 0;
}
} // namespace

bool traceEnabled()
{
    static const bool enabled = std::getenv("EVE_TRACE") != nullptr;
    return enabled;
}

Chip::Chip()
  : ramG(kRamGSize, 0), ramDL(kRamDLSize, 0), ramCmd(kRamCmdSize, 0),
    framebuffer(static_cast<size_t>(kScreenWidth) * kScreenHeight, 0xFF000000u)
{
    matrix = { 65536, 0, 0, 0, 65536, 0 };
    for (auto& h : handles)
    {
        h.width = 0;
    }
}

//------------------------------------------------------------------------------------
// Memory map

uint8_t Chip::readByte(uint32_t address) const
{
    address &= kAddressMask;
    if (address < kRamGSize)
    {
        return ramG[address];
    }
    if (address >= kRamDL && address < kRamDL + kRamDLSize)
    {
        return ramDL[address - kRamDL];
    }
    if (address >= kRamCmd && address < kRamCmd + kRamCmdSize)
    {
        return ramCmd[address - kRamCmd];
    }
    return 0;
}

void Chip::writeByte(uint32_t address, uint8_t value)
{
    address &= kAddressMask;
    if (address < kRamGSize)
    {
        ramG[address] = value;
    }
    else if (address >= kRamDL && address < kRamDL + kRamDLSize)
    {
        ramDL[address - kRamDL] = value;
    }
    else if (address >= kRamCmd && address < kRamCmd + kRamCmdSize)
    {
        ramCmd[address - kRamCmd] = value;
    }
}

uint32_t Chip::readWord(uint32_t address) const
{
    return readByte(address) | (readByte(address + 1) << 8) | (readByte(address + 2) << 16) |
           (static_cast<uint32_t>(readByte(address + 3)) << 24);
}

void Chip::writeWord(uint32_t address, uint32_t value)
{
    for (int i = 0; i < 4; i++)
    {
        writeByte(address + i, static_cast<uint8_t>(value >> (8 * i)));
    }
}

void Chip::writeMemory(uint32_t address, const uint8_t* data, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        writeByte(address + static_cast<uint32_t>(i), data[i]);
    }
}

void Chip::readMemory(uint32_t address, uint8_t* data, size_t size) const
{
    for (size_t i = 0; i < size; i++)
    {
        data[i] = readByte(address + static_cast<uint32_t>(i));
    }
}

//------------------------------------------------------------------------------------
// Command FIFO

void Chip::writeCommandWord(uint32_t word)
{
    uint8_t bytes[4] = { static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
                         static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24) };
    writeCommandBytes(bytes, 4);
}

void Chip::writeCommandBytes(const uint8_t* data, size_t size)
{
    pending.insert(pending.end(), data, data + size);
    // Mirror into RAM_CMD so the host can read results back, as getImgProps does.
    for (size_t i = 0; i < size; i++)
    {
        ramCmd[cmdWriteOffset] = data[i];
        cmdWriteOffset = (cmdWriteOffset + 1) % kRamCmdSize;
    }
}

void Chip::executeCommands()
{
    while (pendingStart < pending.size() && executeOne())
    {
    }
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pendingStart));
    pendingStart = 0;
}

bool Chip::peekWord(size_t index, uint32_t& word) const
{
    const size_t at = pendingStart + index * 4;
    if (at + 4 > pending.size())
    {
        return false;
    }
    word = pending[at] | (pending[at + 1] << 8) | (pending[at + 2] << 16) |
           (static_cast<uint32_t>(pending[at + 3]) << 24);
    return true;
}

// Finds the NUL-terminated string starting firstByte bytes into the pending command and
// returns its length including the terminator, padded to a whole word.
bool Chip::peekString(size_t firstByte, size_t& paddedLength) const
{
    for (size_t i = pendingStart + firstByte; i < pending.size(); i++)
    {
        if (pending[i] == 0)
        {
            const size_t length = i - (pendingStart + firstByte) + 1;
            paddedLength = (length + 3) & ~size_t(3);
            return pendingStart + firstByte + paddedLength <= pending.size();
        }
    }
    return false;
}

void Chip::consume(size_t bytes)
{
    pendingStart += bytes;
    cmdReadOffset = static_cast<uint32_t>((cmdReadOffset + bytes) % kRamCmdSize);
}

// Results go into the command's own parameter slots in RAM_CMD, like on the chip.
void Chip::writeResultWord(size_t parameterIndex, uint32_t value)
{
    const uint32_t at = static_cast<uint32_t>((cmdReadOffset + 4 * (parameterIndex + 1)) % kRamCmdSize);
    for (int i = 0; i < 4; i++)
    {
        ramCmd[(at + i) % kRamCmdSize] = static_cast<uint8_t>(value >> (8 * i));
    }
}

void Chip::emit(uint32_t word)
{
    if (dlOffset + 4 > kRamDLSize)
    {
        statistics.displayListOverflows++;
        return;
    }
    std::memcpy(&ramDL[dlOffset], &word, 4);
    dlOffset += 4;
    statistics.displayListWords++;
}

void Chip::emitSetBitmap(uint32_t source, uint32_t format, uint32_t width, uint32_t height)
{
    const uint32_t stride = (width * bitsPerPixel(format) + 7) / 8;
    emit(dlBitmapSource(source));
    emit(dlBitmapLayout(format, stride, height));
    emit(dlBitmapLayoutH(stride, height));
    emit(dlBitmapSize(NEAREST, BORDER, BORDER, width, height));
    emit(dlBitmapSizeH(width, height));
}

// The chip's ROM fonts (16-31) are not available. Text in them is drawn with the loaded font
// closest in height, which keeps popups and messages readable if not pixel-identical.
int Chip::resolveFont(int font) const
{
    if (font < 0 || font >= 32)
    {
        return -1;
    }
    if (fonts[font].valid)
    {
        return font;
    }
    if (font < 16)
    {
        return -1;
    }
    static const int kRomFontHeights[16] = { 8, 8, 16, 16, 13, 17, 20, 22, 29, 38, 16, 20, 25, 28, 36, 49 };
    const int wanted = kRomFontHeights[font - 16];
    int best = -1;
    int bestDistance = 1 << 30;
    for (int i = 0; i < 32; i++)
    {
        if (!fonts[i].valid)
        {
            continue;
        }
        const int distance = std::abs(static_cast<int>(readWord(fonts[i].metrics + 140)) - wanted);
        if (distance < bestDistance)
        {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

void Chip::emitText(int16_t x, int16_t y, int16_t requestedFont, uint16_t options, const char* text)
{
    const int font = resolveFont(requestedFont);
    if (font < 0)
    {
        return;
    }
    const uint32_t metrics = fonts[font].metrics;
    const int fontHeight = static_cast<int>(readWord(metrics + 140));

    int totalWidth = 0;
    for (const char* c = text; *c != 0; c++)
    {
        const uint8_t ch = static_cast<uint8_t>(*c);
        if (ch < 128)
        {
            totalWidth += readByte(metrics + ch);
        }
    }

    int penX = x;
    int penY = y;
    if (options & OPT_RIGHTX)
    {
        penX -= totalWidth;
    }
    else if (options & OPT_CENTERX)
    {
        penX -= totalWidth / 2;
    }
    if (options & OPT_CENTERY)
    {
        penY -= fontHeight / 2;
    }

    for (const char* c = text; *c != 0; c++)
    {
        const uint8_t ch = static_cast<uint8_t>(*c);
        if (ch >= 128)
        {
            continue;
        }
        const int advance = readByte(metrics + ch);
        if (ch >= fonts[font].firstChar && advance > 0 && ch != ' ')
        {
            emit((kOpGlyph << 24) | (static_cast<uint32_t>(font) << 8) | ch);
            emit((static_cast<uint32_t>(static_cast<uint16_t>(penY)) << 16) |
                 static_cast<uint16_t>(penX));
        }
        penX += advance;
    }
}

bool Chip::continueImageLoad()
{
    // Pull everything available into the image buffer, then see whether it is complete.
    const size_t available = pending.size() - pendingStart;
    const size_t before = imageLoad.data.size();
    imageLoad.data.insert(imageLoad.data.end(),
                          pending.begin() + static_cast<std::ptrdiff_t>(pendingStart),
                          pending.end());

    uint32_t width = 0;
    uint32_t height = 0;
    const size_t length = imageLength(imageLoad.data, width, height);
    if (length == 0)
    {
        consume(available);
        return false;
    }

    // The host pads the image data to a whole number of words.
    const size_t padded = (length + 3) & ~size_t(3);
    if (padded > imageLoad.data.size())
    {
        consume(available);
        return false; // padding not here yet; it is part of the image data
    }
    consume(padded - before);

    // TODO(phase 2): decode with stb_image into RGB565 at imageLoad.destination.
    lastImageWidth = width;
    lastImageHeight = height;
    lastImageEnd = imageLoad.destination + width * height * 2;
    imageLoad.active = false;
    imageLoad.data.clear();
    return true;
}

//------------------------------------------------------------------------------------
// Command execution

bool Chip::executeOne()
{
    if (imageLoad.active)
    {
        return continueImageLoad();
    }

    uint32_t command;
    if (!peekWord(0, command))
    {
        return false;
    }

    if ((command & 0xFFFFFF00u) != 0xFFFFFF00u)
    {
        emit(command);
        consume(4);
        return true;
    }

    statistics.commandWords++;
    if (traceEnabled())
    {
        uint32_t a = 0, b = 0, c = 0;
        peekWord(1, a);
        peekWord(2, b);
        peekWord(3, c);
        std::fprintf(stderr, "cmd %08x %08x %08x %08x dl=%u\n", command, a, b, c, dlOffset);
    }

    // Fixed-size parameters first; commands with strings or data handle their own length.
    uint32_t p[6] = {};
    auto params = [&](int count) {
        for (int i = 0; i < count; i++)
        {
            if (!peekWord(1 + i, p[i]))
            {
                return false;
            }
        }
        return true;
    };

    switch (command)
    {
        case CMD_DLSTART:
            dlOffset = 0;
            consume(4);
            return true;

        case CMD_SWAP:
            renderDisplayList();
            consume(4);
            return true;

        case CMD_TEXT:
        case CMD_BUTTON:
        case CMD_KEYS:
        case CMD_TOGGLE:
        {
            const int fixed = command == CMD_TEXT ? 2 : 3;
            size_t stringLength;
            if (!params(fixed) || !peekString(4 + 4 * fixed, stringLength))
            {
                return false;
            }
            if (command == CMD_TEXT)
            {
                const char* text =
                  reinterpret_cast<const char*>(&pending[pendingStart + 4 + 4 * fixed]);
                emitText(static_cast<int16_t>(p[0] & 0xFFFF), static_cast<int16_t>(p[0] >> 16),
                         static_cast<int16_t>(p[1] & 0xFFFF), static_cast<uint16_t>(p[1] >> 16),
                         text);
            }
            consume(4 + 4 * fixed + stringLength);
            return true;
        }

        case CMD_NUMBER:
        {
            if (!params(3))
            {
                return false;
            }
            const uint16_t options = static_cast<uint16_t>(p[1] >> 16);
            const int minDigits = options & 0x1F;
            int64_t value = (options & OPT_SIGNED) ? static_cast<int64_t>(static_cast<int32_t>(p[2]))
                                                   : static_cast<int64_t>(p[2]);
            const bool negative = value < 0;
            uint64_t magnitude = static_cast<uint64_t>(negative ? -value : value);

            char digits[40];
            int count = 0;
            do
            {
                digits[count++] = "0123456789ABCDEF"[magnitude % numberBase];
                magnitude /= numberBase;
            } while (magnitude != 0 && count < 32);
            while (count < minDigits && count < 32)
            {
                digits[count++] = '0';
            }

            char text[42];
            int n = 0;
            if (negative)
            {
                text[n++] = '-';
            }
            while (count > 0)
            {
                text[n++] = digits[--count];
            }
            text[n] = 0;
            emitText(static_cast<int16_t>(p[0] & 0xFFFF), static_cast<int16_t>(p[0] >> 16),
                     static_cast<int16_t>(p[1] & 0xFFFF), options, text);
            consume(16);
            return true;
        }

        case CMD_GRADIENT:
            if (!params(4))
            {
                return false;
            }
            emit(kOpGradient << 24);
            for (int i = 0; i < 4; i++)
            {
                emit(p[i]);
            }
            consume(20);
            return true;

        case CMD_APPEND:
            if (!params(2))
            {
                return false;
            }
            for (uint32_t i = 0; i + 4 <= p[1]; i += 4)
            {
                emit(readWord(p[0] + i));
            }
            consume(12);
            return true;

        case CMD_MEMCPY:
        {
            if (!params(3))
            {
                return false;
            }
            std::vector<uint8_t> copy(p[2]);
            readMemory(p[1], copy.data(), copy.size());
            writeMemory(p[0], copy.data(), copy.size());
            consume(16);
            return true;
        }

        case CMD_MEMZERO:
        case CMD_MEMSET:
        {
            const bool zero = command == CMD_MEMZERO;
            if (!params(zero ? 2 : 3))
            {
                return false;
            }
            const uint8_t value = zero ? 0 : static_cast<uint8_t>(p[1]);
            const uint32_t count = zero ? p[1] : p[2];
            for (uint32_t i = 0; i < count; i++)
            {
                writeByte(p[0] + i, value);
            }
            consume(zero ? 12 : 16);
            return true;
        }

        case CMD_MEMWRITE:
        {
            if (!params(2))
            {
                return false;
            }
            const size_t padded = (p[1] + 3) & ~uint32_t(3);
            if (pendingStart + 12 + padded > pending.size())
            {
                return false;
            }
            writeMemory(p[0], &pending[pendingStart + 12], p[1]);
            consume(12 + padded);
            return true;
        }

        case CMD_SETFONT:
        case CMD_SETFONT2:
        {
            const bool v2 = command == CMD_SETFONT2;
            if (!params(v2 ? 3 : 2))
            {
                return false;
            }
            const uint32_t font = p[0] & 31;
            fonts[font] = { p[1], static_cast<uint8_t>(v2 ? p[2] : 0), true };
            if (v2)
            {
                // Like the chip, set up the bitmap handle so CELL(ch) selects character ch.
                const uint32_t format = readWord(p[1] + 128);
                const uint32_t stride = readWord(p[1] + 132);
                const uint32_t width = readWord(p[1] + 136);
                const uint32_t height = readWord(p[1] + 140);
                const uint32_t raw = readWord(p[1] + 144);
                emit(dlBitmapHandle(font));
                emit(dlBitmapSource(raw - p[2] * stride * height));
                emit(dlBitmapLayout(format, stride, height));
                emit(dlBitmapLayoutH(stride, height));
                emit(dlBitmapSize(NEAREST, BORDER, BORDER, width, height));
                emit(dlBitmapSizeH(width, height));
            }
            consume(v2 ? 16 : 12);
            return true;
        }

        case CMD_SETBITMAP:
            if (!params(3))
            {
                return false;
            }
            emitSetBitmap(p[0], p[1] & 0xFFFF, p[1] >> 16, p[2] & 0xFFFF);
            consume(16);
            return true;

        case CMD_SETBASE:
            if (!params(1))
            {
                return false;
            }
            numberBase = (p[0] >= 2 && p[0] <= 16) ? static_cast<int>(p[0]) : 10;
            consume(8);
            return true;

        case CMD_LOADIDENTITY:
            matrix = { 65536, 0, 0, 0, 65536, 0 };
            consume(4);
            return true;

        case CMD_TRANSLATE:
        case CMD_SCALE:
        case CMD_ROTATE:
        {
            const bool rotate = command == CMD_ROTATE;
            if (!params(rotate ? 1 : 2))
            {
                return false;
            }
            // Post-multiply the forward (bitmap -> screen) matrix by the operation.
            double op[6] = { 1, 0, 0, 0, 1, 0 };
            if (command == CMD_TRANSLATE)
            {
                op[2] = static_cast<int32_t>(p[0]) / 65536.0;
                op[5] = static_cast<int32_t>(p[1]) / 65536.0;
            }
            else if (command == CMD_SCALE)
            {
                op[0] = static_cast<int32_t>(p[0]) / 65536.0;
                op[4] = static_cast<int32_t>(p[1]) / 65536.0;
            }
            else
            {
                // Clockwise on screen (y grows downwards); a full turn is 65536.
                const double angle = static_cast<int32_t>(p[0]) * 2.0 * M_PI / 65536.0;
                op[0] = std::cos(angle);
                op[1] = -std::sin(angle);
                op[3] = std::sin(angle);
                op[4] = std::cos(angle);
            }
            double m[6];
            for (int i = 0; i < 6; i++)
            {
                m[i] = (i == 2 || i == 5) ? matrix[i] / 65536.0 : matrix[i] / 65536.0;
            }
            const double r[6] = {
                m[0] * op[0] + m[1] * op[3],        m[0] * op[1] + m[1] * op[4],
                m[0] * op[2] + m[1] * op[5] + m[2], m[3] * op[0] + m[4] * op[3],
                m[3] * op[1] + m[4] * op[4],        m[3] * op[2] + m[4] * op[5] + m[5],
            };
            for (int i = 0; i < 6; i++)
            {
                matrix[i] = static_cast<int64_t>(std::llround(r[i] * 65536.0));
            }
            consume(rotate ? 8 : 12);
            return true;
        }

        case CMD_SETMATRIX:
        {
            // The graphics engine maps screen to bitmap, so it gets the inverse matrix.
            const double a = matrix[0] / 65536.0, b = matrix[1] / 65536.0, c = matrix[2] / 65536.0;
            const double d = matrix[3] / 65536.0, e = matrix[4] / 65536.0, f = matrix[5] / 65536.0;
            const double det = a * e - b * d;
            if (std::fabs(det) > 1e-12)
            {
                const double ia = e / det, ib = -b / det, id = -d / det, ie = a / det;
                const double ic = -(ia * c + ib * f), iff = -(id * c + ie * f);
                auto fixed88 = [](double v) { return static_cast<uint32_t>(std::lround(v * 256.0)); };
                emit((21u << 24) | (fixed88(ia) & 0x1FFFF));
                emit((22u << 24) | (fixed88(ib) & 0x1FFFF));
                emit((23u << 24) | (fixed88(ic) & 0xFFFFFF));
                emit((24u << 24) | (fixed88(id) & 0x1FFFF));
                emit((25u << 24) | (fixed88(ie) & 0x1FFFF));
                emit((26u << 24) | (fixed88(iff) & 0xFFFFFF));
            }
            consume(4);
            return true;
        }

        case CMD_GETMATRIX:
            if (!params(6))
            {
                return false;
            }
            for (int i = 0; i < 6; i++)
            {
                writeResultWord(i, static_cast<uint32_t>(matrix[i]));
            }
            consume(28);
            return true;

        case CMD_LOADIMAGE:
            if (!params(2))
            {
                return false;
            }
            imageLoad.active = true;
            imageLoad.destination = p[0];
            imageLoad.data.clear();
            consume(12);
            return true;

        case CMD_GETPROPS:
            if (!params(3))
            {
                return false;
            }
            writeResultWord(0, lastImageEnd);
            writeResultWord(1, lastImageWidth);
            writeResultWord(2, lastImageHeight);
            consume(16);
            return true;

        case CMD_GETPTR:
            if (!params(1))
            {
                return false;
            }
            writeResultWord(0, lastImageEnd);
            consume(8);
            return true;

        case CMD_SNAPSHOT2:
            if (!params(4))
            {
                return false;
            }
            snapshot(p[0], p[1], static_cast<int16_t>(p[2] & 0xFFFF), static_cast<int16_t>(p[2] >> 16),
                     static_cast<int16_t>(p[3] & 0xFFFF), static_cast<int16_t>(p[3] >> 16));
            consume(20);
            return true;

        // Accepted and ignored: colours of unused widgets, timing and setup commands.
        case CMD_BGCOLOR:
        case CMD_FGCOLOR:
        case CMD_GRADCOLOR:
        case CMD_SETROTATE:
        case CMD_INTERRUPT:
        case CMD_SETSCRATCH:
        case CMD_CALIBRATE:
            if (!params(1))
            {
                return false;
            }
            consume(8);
            return true;

        case CMD_COLDSTART:
        case CMD_STOP:
        case CMD_SYNC:
        case CMD_SCREENSAVER:
        case CMD_LOGO:
        case CMD_VIDEOSTART:
            consume(4);
            return true;

        default:
            // Unknown length: skip the command word and hope the rest is in sync.
            if (statistics.unknownCommands++ == 0 || statistics.lastUnknownCommand != command)
            {
                std::fprintf(stderr, "eve: unsupported coprocessor command 0x%08x\n", command);
            }
            statistics.lastUnknownCommand = command;
            consume(4);
            return true;
    }
}
} // namespace eve
