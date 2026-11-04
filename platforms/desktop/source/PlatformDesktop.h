#pragma once

// Desktop implementation of core's Platform interfaces.
//
// The display is a software FT812 (DesktopDisplayDriver), the SD card a host directory
// (DesktopFileSystem) and the buttons are fed by the front end's input layer
// (DesktopButtons). Everything else except the clock is still a safe stub; later phases
// replace them: MIDI (phase 6). Audio output is the front end's (audio/AudioRenderer.h);
// DesktopCodec only turns the firmware's volume and mute into a gain.

#include "ConfigManager.h"
#include "DesktopButtons.h"
#include "DesktopDisplayDriver.h"
#include "DesktopFileSystem.h"
#include "Platform.h"

#include <atomic>
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

// The Mini has no pad grid; this group never fires.
class StubSwitchGroup : public SwitchGroup
{
  public:
    void setHoldTime(int) override {}
    bool isButtonPressed(int) override { return false; }
    void setActionCallback(ActionType action, void (*callback)(uint8_t)) override;

  private:
    void (*callbacks[3])(uint8_t) = {};
};

// The SGTL5000's output stage as a gain on the rendered audio. Volume follows the codec's
// headphone curve (0.5 dB steps, 100 % = +12 dB down to -51.5 dB); the firmware's default
// of 81 % is its 0 dB point. Above that the hardware boosts the analog output, which would
// clip here, so the gain stops at 0 dB.
//
// Input gain is the codec's ADC stage, 0 to 22.5 dB in 1.5 dB steps, for line in and mic
// alike. The mic preamp (+20 to +40 dB) is left out: a computer's microphone input arrives
// already amplified, and the system's input volume plays that part. DAP and EQ are not
// modelled.
class DesktopCodec : public Sgtl5000
{
  public:
    void init() override {}
    void setInputType(InputType input) override;
    void setHeadphoneSource(HeadphoneSource) override {}
    void setLineInLevel(int level) override;
    void setMicGain(int gainDb) override;
    void setHeadphoneMuted(bool muted) override { headphoneMuted = muted; }
    void setMasterVolume(float volume) override;
    void setAutoVolumeEnabled(bool) override {}
    void configureAutoVolume(AutoVolumeParams*) override {}
    void setEnhanceBassEnabled(bool) override {}
    void configureEnhanceBass(EnhanceBassParams*) override {}
    void setEqualizerType(EqualizerType) override {}
    void configureFiveBandEqualizer(float*) override {}
    void enableSurroundSound(bool) override {}
    void configureSurroundSound(SurroundSoundParams*) override {}
    void enableHardwareProcessor(bool) override {}

    // Linear gain for the output, 0 when muted. Safe to read from the audio thread.
    float outputGain() const { return headphoneMuted ? 0.0f : volumeGain.load(std::memory_order_relaxed); }
    // Linear gain for the input. Safe to read from the audio thread.
    float inputGain() const { return inputLinearGain.load(std::memory_order_relaxed); }

    // After arming, the next input setting (the Sample Recorder configuring its source)
    // raises a request the front end takes to open the capture device. Core's own input
    // set-up at start-up comes before arming.
    void armInputRequests() { inputArmed = true; }
    bool takeInputRequest() { return inputRequested.exchange(false); }

  private:
    void noteInputUse();
    void updateInputGain();

    InputType inputType = InputType::kLineIn;
    int lineInStage = 0; // ADC gain steps, 0..15
    int micStage = 0;
    std::atomic<float> inputLinearGain{ 1.0f };
    bool inputArmed = false;
    std::atomic<bool> inputRequested{ false };

    std::atomic<bool> headphoneMuted{ false };
    std::atomic<float> volumeGain{ 1.0f };
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

    DesktopButtons& desktopButtons() { return buttons; }
    DesktopCodec& desktopCodec() { return codec; }
    DesktopDisplayDriver& desktopDisplay() { return display; }
    DesktopFileSystem& desktopFileSystem() { return fileSystem; }
    bool powerOffRequested() const { return powerOff; }
    // The Mini's power button: held for 1.5 s, core saves the project and powers off.
    void setPowerButton(bool pressed);
    bool isPowerButtonPressed() const { return powerButtonPressed; }

  private:
    DesktopTimer timer;
    DesktopCodec codec;
    StubRadio radio;
    DesktopButtons buttons;
    StubSwitchGroup pads;
    StubLeds leds;
    StubBattery battery;
    StubEncoder encoder;
    DesktopDisplayDriver display;
    StubUsbAudio usbAudio;
    StubUartMidi uartMidi;
    StubUsbMidi usbMidi;
    DesktopFileSystem fileSystem;
    FileConfigManager configManager;
    ZeroCrcCalculator crc;
    SystemInfo systemInfo;
    StubUsb usb;

    PowerButtonCallback powerButtonCallback = nullptr;
    bool powerOff = false;
    bool powerButtonPressed = false;
};
