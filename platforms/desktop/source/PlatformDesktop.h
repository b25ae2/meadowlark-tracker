#pragma once

// Desktop implementation of core's Platform interfaces.
//
// Phase 0: everything except the clock is a safe stub, so core can init and run
// headless. Later phases replace the stubs: display renderer (phase 2), audio
// (phase 3), input (phase 4), file system (phase 5), MIDI (phase 6).

#include "ConfigManager.h"
#include "DisplayDriver.h"
#include "Platform.h"

#include <chrono>
#include <cstdint>

class DesktopTimer : public Timer
{
  public:
    DesktopTimer();
    uint32_t millis() override;
    uint32_t micros() override;
    void delay(int ms) override;
    // Reported in nanoseconds; Profiler only uses differences divided by clockCyclesPerUs().
    uint32_t clockCycles() override;
    uint32_t clockCyclesPerUs() override { return 1000; }

  private:
    std::chrono::steady_clock::time_point start;
};

// Accepts the FT812 command stream and discards it, counting what the UI sends.
class StubDisplayDriver : public DisplayDriver
{
  public:
    void setPowerState(bool) override {}
    void setBrightness(int) override {}
    void setRotation(int) override {}

    void getImgProps(uint32_t& addr, uint32_t& width, uint32_t& height) override;
    uint32_t getDisplayListOffset() override { return 0; }
    void IncCMDOffset(uint16_t) override {}
    void WriteData32(uint32_t) override { wordsWritten++; }

    void beginCoProList() override {}
    void beginCoProListNoCheck() override {}
    void endCoProList() override { listsSubmitted++; }
    void awaitCoProEmpty() override {}
    uint8_t isCoProEmpty() override { return 1; }
    void writeDataRAMG(const uint8_t*, uint32_t dataSize, uint32_t) override;
    void writeFontRAMG(const uint8_t*, uint32_t dataSize, uint32_t) override;
    void readDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t) override;
    void readBackDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t) override;
    uint8_t sendString(const char* string, uint32_t length) override;
    void writeDataToCMD(const uint8_t*, uint32_t totalDataSize) override;

    void initStreamDma(uint16_t, uint16_t, uint16_t, uint8_t*, uint32_t) override {}
    bool updateStreamDma(bool) override { return true; }
    void stopStreamDma() override {}

    uint64_t wordsWritten = 0;
    uint64_t listsSubmitted = 0;
    uint64_t ramgBytesWritten = 0;
};

class StubSwitchGroup : public SwitchGroup
{
  public:
    void setHoldTime(int) override {}
    bool isButtonPressed(int) override { return false; }
    void setActionCallback(ActionType action, void (*callback)(uint8_t)) override;

  private:
    void (*callbacks[3])(uint8_t) = {};
};

class StubSgtl5000 : public Sgtl5000
{
  public:
    void init() override {}
    void setInputType(InputType) override {}
    void setHeadphoneSource(HeadphoneSource) override {}
    void setLineInLevel(int) override {}
    void setMicGain(int) override {}
    void setHeadphoneMuted(bool) override {}
    void setMasterVolume(float) override {}
    void setAutoVolumeEnabled(bool) override {}
    void configureAutoVolume(AutoVolumeParams*) override {}
    void setEnhanceBassEnabled(bool) override {}
    void configureEnhanceBass(EnhanceBassParams*) override {}
    void setEqualizerType(EqualizerType) override {}
    void configureFiveBandEqualizer(float*) override {}
    void enableSurroundSound(bool) override {}
    void configureSurroundSound(SurroundSoundParams*) override {}
    void enableHardwareProcessor(bool) override {}
};

class StubRadio : public Radio
{
  public:
    void enableLineInput(bool) override {}
    void enable(bool) override {}
    bool setFrequency(float) override { return false; }
    void setSeekCallback(void (*)(void)) override {}
    void seek(SeekDirection) override {}
    void updateSeek() override {}
    bool updateRDS() override { return false; }
    float getFrequency() override { return 0.0f; }
    float getBaseFrequency() override { return 87.5f; }
    float getSpacing() override { return 0.1f; }
    bool isinitialized() override { return false; }
    void clearRDS() override {}
    void setRegion(Region) override {}
    bool isRegionChanged() override { return false; }
    char* getStationName() override { return stationName; }

  private:
    char stationName[1] = { 0 };
};

class StubLeds : public Leds
{
  public:
    void setLed(int, bool, int) override {}
};

// Desktop runs on mains power: always full and not charging.
class StubBattery : public Battery
{
  public:
    void enableMonitoring() override {}
    int getLevel() override { return 100; }
    float getVoltage() override { return 4.2f; }
    bool getCharging() override { return false; }
    int getTimeTo() override { return 0; }
    bool getState() override { return true; }
    bool isEmpty() override { return false; }
};

class StubEncoder : public Encoder
{
  public:
    void setAcceleration(int) override {}
    void setResolution(int) override {}
};

class StubUsbAudio : public UsbAudio
{
  public:
    void beginFillData() override {}
    void fillChannelData(void*, int, int) override {}
    void finishFillData() override {}
};

class StubUartMidi : public UartMidi
{
  public:
    bool isReadAvailable() override { return false; }
    void read() override {}
    void sendRealTime(MessageType) override {}
    void sendControlChange(uint8_t, uint8_t, uint8_t) override {}
    void sendProgramChange(uint8_t, uint8_t) override {}
    void sendAfterTouch(uint8_t, uint8_t) override {}
    void sendPolyPressure(uint8_t, uint8_t, uint8_t) override {}
    void sendPitchBend(int, uint8_t) override {}
    void sendNoteOn(uint8_t, uint8_t, uint8_t) override {}
    void sendNoteOff(uint8_t, uint8_t, uint8_t) override {}
    void init() override {}
    void setReceiveActions(MidiReceiveActions*) override {}
    void setReceiveNoteOnAction(void (*)(uint8_t, uint8_t, uint8_t)) override {}
};

class StubUsbMidi : public UsbMidi
{
  public:
    bool isReadAvailable() override { return false; }
    void read() override {}
    void sendRealTime(MessageType) override {}
    void sendControlChange(uint8_t, uint8_t, uint8_t) override {}
    void sendProgramChange(uint8_t, uint8_t) override {}
    void sendAfterTouch(uint8_t, uint8_t) override {}
    void sendPolyPressure(uint8_t, uint8_t, uint8_t) override {}
    void sendPitchBend(int, uint8_t) override {}
    void sendNoteOn(uint8_t, uint8_t, uint8_t) override {}
    void sendNoteOff(uint8_t, uint8_t, uint8_t) override {}
    void setReceiveActions(MidiReceiveActions*) override {}
};

// No card inserted: every operation fails cleanly. Replaced by the host-directory
// file system in phase 5.
class StubFileSystem : public FileSystem
{
  public:
    bool isSdCardInserted() override { return false; }
    void setMountEventCallback(CardStateChangeCallback) override {}

    bool exists(const char*) override { return false; }
    bool rename(const char*, const char*) override { return false; }
    bool mkdir(uint8_t, const char*, bool) override { return false; }
    bool remove(const char*) override { return false; }
    bool removeDirWithFiles(const char*) override { return false; }
    bool removeOnlyDirFiles(const char*) override { return false; }

    size_t getFileSize(const char*) override { return 0; }
    size_t getFileSize(FileHandle) override { return 0; }
    size_t getFileAvailable(FileHandle) override { return 0; }

    std::tuple<bool, FileHandle> openFile(const char*, FileOpenMode, FileHandle*) override
    {
        return { false, nullptr };
    }
    size_t readFile(void*, size_t, FileHandle) override { return 0; }
    size_t writeFile(const void*, size_t, FileHandle) override { return 0; }
    bool seekFileCur(int, FileHandle) override { return false; }
    bool seekFile(uint32_t, FileHandle) override { return false; }
    void closeFile(FileHandle, bool) override {}
    void endFile(FileHandle) override {}
    bool isFileOpen(FileHandle) override { return false; }
    bool enableFastSeek(FileHandle, uint32_t*, uint32_t, uint32_t*, uint32_t*) override
    {
        return false;
    }
    bool readSectorsAsync(uint8_t*, uint32_t, uint32_t) override { return false; }
    bool pollAsyncRead(bool* outSuccess) override;
    bool waitAsyncRead() override { return false; }
    bool isAsyncReadBusy() override { return false; }

    int createInstrumentsList(const char*, char**, uint8_t, uint32_t*) override { return 0; }
    int countContainedFiles(const char*) override { return 0; }
    int createFilesList(const char*,
                        uint8_t,
                        char**,
                        uint8_t,
                        uint16_t,
                        enListFileFilter,
                        uint16_t,
                        uint16_t,
                        enListFileFilter) override
    {
        return 0;
    }
    void listDirContents(const char*,
                         DirContentType,
                         std::function<bool(const char* name)>,
                         char*,
                         int,
                         int,
                         int& outNamesCount,
                         char**) override
    {
        outNamesCount = 0;
    }

    int readNbFiles(const char*) override { return 0; }
    bool nesDir_open(const char*) override { return false; }
    bool nesDir_readItem(char*, uint8_t*, uint8_t*, uint32_t*) override { return false; }
    void nesDir_close() override {}
};

// Same as the Mini: settings live in /trackerConfig on the "SD card".
class FileConfigManager : public ConfigManager
{
  public:
    explicit FileConfigManager(FileSystem* fileSystem) : fileSystem(fileSystem) {}
    bool readData(int address, void* data, int size) override;
    void writeData(int address, void* data, int size) override;

  private:
    FileSystem* fileSystem;
};

// The Tracker+/Mini firmware has no usable CRC hardware and writes 0 into every
// saved file; desktop must do the same for files to round-trip with hardware.
class ZeroCrcCalculator : public CrcCalculator
{
  public:
    uint32_t calculate32bitCrc(const uint8_t*, const uint16_t) override { return 0; }
};

class StubUsb : public Usb
{
  public:
    void init(void) override {}
    void reset(void) override {}
    void update(void) override {}
    void setClassState(ClassType, bool) override {}
    bool getClassState(ClassType) override { return false; }
    bool isAttached() override { return false; }
};

class PlatformDesktop : public Platform
{
  public:
    PlatformDesktop();

    void setPowerMode(bool mode) override;
    void resetMCU() override;
    void runBootloader(char* fileName) override;
    void setPowerButtonCallback(PowerButtonCallback callback) override;
    bool getPowerButtonState() override { return true; }
    // true skips the Mini's "power off unless the button is held" startup check.
    bool readForceStartUp() override { return true; }

    Sgtl5000* getCodecAudio() override { return &codec; }
    Radio* getRadio() override { return &radio; }
    SwitchGroup* getButtons() override { return &buttons; }
    SwitchGroup* getPads() override { return &pads; }
    Leds* getLeds() override { return &leds; }
    Battery* getBattery() override { return &battery; }
    Timer* getTimer() override { return &timer; }
    Encoder* getEncoder() override { return &encoder; }
    DisplayDriver* getDisplayDriver() override { return &display; }
    UsbAudio* getUsbAudio() override { return &usbAudio; }
    UartMidi* getUartMidi() override { return &uartMidi; }
    UsbMidi* getUsbMidi() override { return &usbMidi; }
    ConfigManager* getConfigManager() override { return &configManager; }
    FileSystem* getFileSystem() override { return &fileSystem; }
    CrcCalculator* getCrcCalculator() override { return &crc; }
    SystemInfo* getSystemInfo() override { return &systemInfo; }
    Usb* getUsb() override { return &usb; }

    StubDisplayDriver& stubDisplay() { return display; }
    bool powerOffRequested() const { return powerOff; }

  private:
    StubSgtl5000 codec;
    StubRadio radio;
    StubSwitchGroup buttons;
    // The Mini has no pad grid; this group never fires.
    StubSwitchGroup pads;
    StubLeds leds;
    StubBattery battery;
    DesktopTimer timer;
    StubEncoder encoder;
    StubDisplayDriver display;
    StubUsbAudio usbAudio;
    StubUartMidi uartMidi;
    StubUsbMidi usbMidi;
    StubFileSystem fileSystem;
    FileConfigManager configManager;
    ZeroCrcCalculator crc;
    SystemInfo systemInfo;
    StubUsb usb;

    PowerButtonCallback powerButtonCallback = nullptr;
    bool powerOff = false;
};
