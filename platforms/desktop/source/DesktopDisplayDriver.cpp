#include "DesktopDisplayDriver.h"

#include <cstring>
#include <vector>

void DesktopDisplayDriver::endCoProList()
{
    chip.executeCommands();
    listsSubmitted++;
}

// Like the firmware: the three words CMD_GETPROPS left just before the FIFO write pointer.
void DesktopDisplayDriver::getImgProps(uint32_t& addr, uint32_t& width, uint32_t& height)
{
    const uint32_t write = chip.commandWriteOffset();
    auto word = [&](uint32_t back) {
        uint32_t value = 0;
        const uint32_t offset = (write - back) & (eve::kRamCmdSize - 1);
        uint8_t bytes[4];
        chip.readMemory(eve::kRamCmd + offset, bytes, 4);
        std::memcpy(&value, bytes, 4);
        return value;
    };
    addr = word(12);
    width = word(8);
    height = word(4);
}

void DesktopDisplayDriver::writeDataRAMG(const uint8_t* imgData, uint32_t dataSize, uint32_t destAddress)
{
    chip.writeMemory(destAddress, imgData, dataSize);
    ramgBytesWritten += dataSize;
}

// The metric block's last word (offset 144) is the glyph data pointer; it is written as
// the block's own address + 148, where the glyphs follow, like API_LIB_WriteFontRAMG.
void DesktopDisplayDriver::writeFontRAMG(const uint8_t* imgData, uint32_t dataSize, uint32_t destAddress)
{
    std::vector<uint8_t> copy(imgData, imgData + dataSize);
    if (dataSize >= 148)
    {
        const uint32_t pointer = destAddress + 148;
        std::memcpy(&copy[144], &pointer, 4);
    }
    chip.writeMemory(destAddress, copy.data(), copy.size());
    ramgBytesWritten += dataSize;
}

void DesktopDisplayDriver::readDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t sourceAddress)
{
    chip.readMemory(sourceAddress, imgData, dataSize);
}

// API_LIB_ReadBackDataRAMG starts reading one byte before sourceAddress and stores the bytes
// from the end of the buffer backwards.
void DesktopDisplayDriver::readBackDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t sourceAddress)
{
    std::vector<uint8_t> forward(dataSize);
    chip.readMemory(sourceAddress - 1, forward.data(), dataSize);
    for (uint32_t i = 0; i < dataSize; i++)
    {
        imgData[dataSize - 1 - i] = forward[i];
    }
}

// Same length rules and padding as API_SendString.
uint8_t DesktopDisplayDriver::sendString(const char* string, uint32_t length)
{
    const uint32_t realLength = static_cast<uint32_t>(std::strlen(string));
    if (length == 0 || length > realLength)
    {
        length = realLength;
    }
    std::vector<uint8_t> bytes(string, string + length);
    bytes.push_back(0);
    while (bytes.size() % 4 != 0)
    {
        bytes.push_back(0);
    }
    chip.writeCommandBytes(bytes.data(), bytes.size());
    return static_cast<uint8_t>(bytes.size());
}

void DesktopDisplayDriver::writeDataToCMD(const uint8_t* imgData, uint32_t totalDataSize)
{
    chip.writeCommandBytes(imgData, totalDataSize);
    static const uint8_t zeros[3] = {};
    if (totalDataSize % 4 != 0)
    {
        chip.writeCommandBytes(zeros, 4 - totalDataSize % 4);
    }
}

void DesktopDisplayDriver::initStreamDma(uint16_t byteResolution,
                                         uint16_t streamWidth,
                                         uint16_t streamHeight,
                                         uint8_t* streamBuf,
                                         uint32_t destMemAddress)
{
    streamStep = 0;
    if (streamInitialized)
    {
        return;
    }
    streamDestination = destMemAddress;
    streamSize = static_cast<uint32_t>(byteResolution) * streamWidth * streamHeight;
    streamBuffer = streamBuf;
    streamInitialized = true;
}

// Same step sequence as the DMA version; the "transfer" completes immediately.
bool DesktopDisplayDriver::updateStreamDma(bool lastFrame)
{
    if (!streamInitialized)
    {
        return true;
    }
    if (streamStep == 0 && !lastFrame)
    {
        return false;
    }
    switch (streamStep)
    {
        case 0:
            chip.writeMemory(streamDestination, streamBuffer, streamSize);
            streamStep = 2;
            return false;
        case 2: streamStep = 3; return true;
        case 3: streamStep = 0; return false;
        default: return false;
    }
}
