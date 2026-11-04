#pragma once

// The firmware's two MIDI ports on the desktop: "usb" (the Mini's USB MIDI device) and
// "jack" (its TRS MIDI in/out). Core talks to them through UsbMidi and UartMidi, as on the
// hardware; the front end connects them to the system's MIDI (midi/MidiBackend.h).
//
// Incoming messages are queued by the backend's thread and handed to core when it polls,
// once per audio block (midiUpdate() in updateAudioBuffers). Outgoing ones go to a Sink,
// which times them to the audio (midi/MidiScheduler.h); without one they are dropped.

#include "Platform.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace midi
{
enum Port : int
{
    kPortUsb = 0,
    kPortJack = 1,
    kPortCount = 2,
};

// "usb" or "jack", as in midi.toml.
const char* portName(int port);

// A channel or system message without SysEx: a status byte and up to two data bytes.
// Channels in status bytes are 0-based; core's API uses 1-16.
struct Message
{
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

// Bytes in a message that starts with this status: 1 to 3, or 0 for SysEx and the
// undefined system bytes, which are not handled.
size_t messageSize(uint8_t status);

// Where outgoing messages go. Called from the render thread and the main loop.
class Sink
{
  public:
    virtual ~Sink() = default;
    // delayUs: how long after the start of the audio block being rendered the message
    // belongs (a MIDI clock in the middle of a block); 0 otherwise.
    virtual void send(int port, const Message& message, uint32_t delayUs) = 0;
};

// Incoming messages for one port: any thread pushes, core's poll takes them. The poll never
// waits for the lock; whatever it can't take now comes with the next audio block.
class InQueue
{
  public:
    static constexpr size_t kCapacity = 512;

    // Drops the message when the queue is full.
    void push(const Message& message);
    bool tryPop(Message& message);
    uint64_t dropped() const { return droppedCount.load(std::memory_order_relaxed); }

  private:
    std::mutex mutex;
    Message ring[kCapacity];
    size_t head = 0;
    size_t count = 0;
    std::atomic<uint64_t> droppedCount{ 0 };
};

// Shared by both ports: the queue, the sink, and message encoding.
class PortIo
{
  public:
    explicit PortIo(int port) : port(port) {}

    InQueue& input() { return queue; }
    void setSink(Sink* sink) { this->sink.store(sink, std::memory_order_release); }

    // isReadAvailable() and read(): fetch() holds the next message until take().
    bool fetch();
    Message take();

    void send(uint8_t status, uint8_t channel, uint8_t data1, uint8_t data2);
    void sendRealTime(uint8_t status);
    void sendPitchBend(int value, uint8_t channel);

  private:
    const int port;
    InQueue queue;
    std::atomic<Sink*> sink{ nullptr };
    Message pending;
    bool hasPending = false;
};

// UsbMidiImpl on IMX: note on with velocity 0 is passed through (core turns it into a note
// off), and only clock, start, stop and continue reach the real-time handler.
class DesktopUsbMidi : public UsbMidi
{
  public:
    PortIo& io() { return portIo; }

    bool isReadAvailable() override { return portIo.fetch(); }
    void read() override;
    void sendRealTime(MessageType messageType) override;
    void sendControlChange(uint8_t index, uint8_t value, uint8_t channel) override;
    void sendProgramChange(uint8_t programNumber, uint8_t channel) override;
    void sendAfterTouch(uint8_t velocity, uint8_t channel) override;
    void sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel) override;
    void sendPitchBend(int value, uint8_t channel) override;
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void setReceiveActions(MidiReceiveActions* receiveActions) override;

  private:
    PortIo portIo{ kPortUsb };
    MidiReceiveActions actions{};
};

// UartMidiImpl on IMX, which runs the Arduino MIDI library in omni mode with thru off:
// note on with velocity 0 calls the note off handler.
class DesktopUartMidi : public UartMidi
{
  public:
    PortIo& io() { return portIo; }

    void init() override {}
    bool isReadAvailable() override { return portIo.fetch(); }
    void read() override;
    void sendRealTime(MessageType messageType) override;
    void sendControlChange(uint8_t index, uint8_t value, uint8_t channel) override;
    void sendProgramChange(uint8_t programNumber, uint8_t channel) override;
    void sendAfterTouch(uint8_t velocity, uint8_t channel) override;
    void sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel) override;
    void sendPitchBend(int value, uint8_t channel) override;
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void setReceiveActions(MidiReceiveActions* receiveActions) override;
    void setReceiveNoteOnAction(void (*action)(uint8_t, uint8_t, uint8_t)) override;

  private:
    PortIo portIo{ kPortJack };
    MidiReceiveActions actions{};
};
} // namespace midi
