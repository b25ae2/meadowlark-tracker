#pragma once

// core's DisplayDriver on top of the software FT812 (eve::Chip).
//
// Mirrors platforms/imx/source/DisplayDriverImpl.cpp and its API_Layer: the same writes,
// padding and read-back conventions, with SPI transfers replaced by direct memory access.
// Command lists run when they are published (endCoProList), so awaitCoProEmpty never waits.

#include "DisplayDriver.h"
#include "eve/EveChip.h"

class DesktopDisplayDriver : public DisplayDriver
{
  public:
    void setPowerState(bool state) override { poweredOn = state; }
    void setBrightness(int value) override { brightness = value; }
    void setRotation(int value) override { rotation = value; }

    void getImgProps(uint32_t& addr, uint32_t& width, uint32_t& height) override;

    uint32_t getDisplayListOffset() override { return chip.displayListOffset(); }
    // The firmware tracks the FIFO write offset itself; here the chip counts real bytes.
    void IncCMDOffset(uint16_t) override {}
    void WriteData32(uint32_t data) override { chip.writeCommandWord(data); }

    void beginCoProList() override {}
    void beginCoProListNoCheck() override {}
    void endCoProList() override;
    void awaitCoProEmpty() override {}
    uint8_t isCoProEmpty() override { return 1; }

    void writeDataRAMG(const uint8_t* imgData, uint32_t dataSize, uint32_t destAddress) override;
    void writeFontRAMG(const uint8_t* imgData, uint32_t dataSize, uint32_t destAddress) override;
    void readDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t sourceAddress) override;
    void readBackDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t sourceAddress) override;
    uint8_t sendString(const char* string, uint32_t length) override;
    void writeDataToCMD(const uint8_t* imgData, uint32_t totalDataSize) override;

    void initStreamDma(uint16_t byteResolution,
                       uint16_t streamWidth,
                       uint16_t streamHeight,
                       uint8_t* streamBuf,
                       uint32_t destMemAddress) override;
    bool updateStreamDma(bool lastFrame) override;
    void stopStreamDma() override { streamInitialized = false; }

    const eve::Chip& eve() const { return chip; }
    int getBrightness() const { return brightness; }
    bool isPoweredOn() const { return poweredOn; }

    uint64_t listsSubmitted = 0;
    uint64_t ramgBytesWritten = 0;

  private:
    eve::Chip chip;
    bool poweredOn = true;
    int brightness = 128;
    int rotation = 0;

    bool streamInitialized = false;
    int streamStep = 0;
    uint32_t streamDestination = 0;
    uint32_t streamSize = 0;
    const uint8_t* streamBuffer = nullptr;
};
