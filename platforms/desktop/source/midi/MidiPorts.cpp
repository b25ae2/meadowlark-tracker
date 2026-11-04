#include "MidiPorts.h"

#include "PlatformDesktop.h"

namespace midi
{
const char* portName(int port)
{
    return port == kPortJack ? "jack" : "usb";
}

size_t messageSize(uint8_t status)
{
    if (status < 0x80)
        return 0;
    if (status < 0xF0)
    {
        const uint8_t type = status & 0xF0;
        return type == 0xC0 || type == 0xD0 ? 2 : 3;
    }
    switch (status)
    {
        case 0xF1: // time code quarter frame
        case 0xF3: // song select
            return 2;
        case 0xF2: // song position
            return 3;
        case 0xF6: // tune request
        case 0xF8:
        case 0xFA:
        case 0xFB:
        case 0xFC:
        case 0xFE:
        case 0xFF:
            return 1;
        default: // SysEx and undefined
            return 0;
    }
}

//------------------------------------------------------------------------------
// InQueue

void InQueue::push(const Message& message)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (count == kCapacity)
    {
        droppedCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ring[(head + count) % kCapacity] = message;
    count++;
}

bool InQueue::tryPop(Message& message)
{
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock() || count == 0)
        return false;
    message = ring[head];
    head = (head + 1) % kCapacity;
    count--;
    return true;
}

//------------------------------------------------------------------------------
// PortIo

bool PortIo::fetch()
{
    if (!hasPending)
        hasPending = queue.tryPop(pending);
    return hasPending;
}

Message PortIo::take()
{
    hasPending = false;
    return pending;
}

void PortIo::send(uint8_t status, uint8_t channel, uint8_t data1, uint8_t data2)
{
    Sink* target = sink.load(std::memory_order_acquire);
    if (!target)
        return;
    Message message;
    message.status = static_cast<uint8_t>(status | ((channel - 1) & 0x0F));
    message.data1 = data1 & 0x7F;
    message.data2 = data2 & 0x7F;
    target->send(port, message, DesktopTimer::midiDelayUs());
}

void PortIo::sendRealTime(uint8_t status)
{
    Sink* target = sink.load(std::memory_order_acquire);
    if (!target)
        return;
    Message message;
    message.status = status;
    target->send(port, message, DesktopTimer::midiDelayUs());
}

void PortIo::sendPitchBend(int value, uint8_t channel)
{
    if (value < -8192)
        value = -8192;
    else if (value > 8191)
        value = 8191;
    value += 8192;
    send(0xE0, channel, static_cast<uint8_t>(value & 0x7F), static_cast<uint8_t>(value >> 7));
}

namespace
{
bool isSentRealTime(Midi::MessageType type)
{
    switch (type)
    {
        case Midi::kClock:
        case Midi::kStart:
        case Midi::kContinue:
        case Midi::kStop:
        case Midi::kActiveSensing:
        case Midi::kSystemReset:
            return true;
        default:
            return false;
    }
}
} // namespace

//------------------------------------------------------------------------------
// DesktopUsbMidi

void DesktopUsbMidi::read()
{
    const Message message = portIo.take();
    const uint8_t channel = (message.status & 0x0F) + 1;
    switch (message.status >= 0xF0 ? message.status : message.status & 0xF0)
    {
        case kClock:
        case kStart:
        case kStop:
        case kContinue:
            if (actions.realTime)
                actions.realTime(message.status);
            break;
        case kNoteOff:
            if (actions.noteOff)
                actions.noteOff(channel, message.data1, message.data2);
            break;
        case kNoteOn:
            if (actions.noteOn)
                actions.noteOn(channel, message.data1, message.data2);
            break;
        case kControlChange:
            if (actions.controlChange)
                actions.controlChange(channel, message.data1, message.data2);
            break;
        case kSongPosition:
            if (actions.songPosition)
                actions.songPosition(static_cast<uint16_t>(message.data1 | (message.data2 << 7)));
            break;
        default:
            break;
    }
}

void DesktopUsbMidi::sendRealTime(MessageType messageType)
{
    if (isSentRealTime(messageType))
        portIo.sendRealTime(static_cast<uint8_t>(messageType));
}

void DesktopUsbMidi::sendControlChange(uint8_t index, uint8_t value, uint8_t channel)
{
    portIo.send(0xB0, channel, index, value);
}

void DesktopUsbMidi::sendProgramChange(uint8_t programNumber, uint8_t channel)
{
    portIo.send(0xC0, channel, programNumber, 0);
}

void DesktopUsbMidi::sendAfterTouch(uint8_t velocity, uint8_t channel)
{
    portIo.send(0xD0, channel, velocity, 0);
}

void DesktopUsbMidi::sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel)
{
    portIo.send(0xA0, channel, note, value);
}

void DesktopUsbMidi::sendPitchBend(int value, uint8_t channel)
{
    portIo.sendPitchBend(value, channel);
}

void DesktopUsbMidi::sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel)
{
    portIo.send(0x90, channel, note, velocity);
}

void DesktopUsbMidi::sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel)
{
    portIo.send(0x80, channel, note, velocity);
}

void DesktopUsbMidi::setReceiveActions(MidiReceiveActions* receiveActions)
{
    actions = *receiveActions;
}

//------------------------------------------------------------------------------
// DesktopUartMidi

void DesktopUartMidi::read()
{
    const Message message = portIo.take();
    const uint8_t channel = (message.status & 0x0F) + 1;
    switch (message.status >= 0xF0 ? message.status : message.status & 0xF0)
    {
        case kNoteOn:
            if (message.data2 != 0)
            {
                if (actions.noteOn)
                    actions.noteOn(channel, message.data1, message.data2);
                break;
            }
            [[fallthrough]];
        case kNoteOff:
            if (actions.noteOff)
                actions.noteOff(channel, message.data1, message.data2);
            break;
        case kControlChange:
            if (actions.controlChange)
                actions.controlChange(channel, message.data1, message.data2);
            break;
        case kClock:
            if (actions.clock)
                actions.clock();
            break;
        case kStart:
            if (actions.start)
                actions.start();
            break;
        case kStop:
            if (actions.stop)
                actions.stop();
            break;
        case kContinue:
            if (actions.continueMessage)
                actions.continueMessage();
            break;
        case kSongPosition:
            if (actions.songPosition)
                actions.songPosition(static_cast<unsigned>(message.data1 | (message.data2 << 7)));
            break;
        default:
            break;
    }
}

void DesktopUartMidi::sendRealTime(MessageType messageType)
{
    if (isSentRealTime(messageType))
        portIo.sendRealTime(static_cast<uint8_t>(messageType));
}

void DesktopUartMidi::sendControlChange(uint8_t index, uint8_t value, uint8_t channel)
{
    portIo.send(0xB0, channel, index, value);
}

void DesktopUartMidi::sendProgramChange(uint8_t programNumber, uint8_t channel)
{
    portIo.send(0xC0, channel, programNumber, 0);
}

void DesktopUartMidi::sendAfterTouch(uint8_t velocity, uint8_t channel)
{
    portIo.send(0xD0, channel, velocity, 0);
}

void DesktopUartMidi::sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel)
{
    portIo.send(0xA0, channel, note, value);
}

void DesktopUartMidi::sendPitchBend(int value, uint8_t channel)
{
    portIo.sendPitchBend(value, channel);
}

void DesktopUartMidi::sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel)
{
    portIo.send(0x90, channel, note, velocity);
}

void DesktopUartMidi::sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel)
{
    portIo.send(0x80, channel, note, velocity);
}

void DesktopUartMidi::setReceiveActions(MidiReceiveActions* receiveActions)
{
    actions = *receiveActions;
}

void DesktopUartMidi::setReceiveNoteOnAction(void (*action)(uint8_t, uint8_t, uint8_t))
{
    actions.noteOn = action;
}
} // namespace midi
