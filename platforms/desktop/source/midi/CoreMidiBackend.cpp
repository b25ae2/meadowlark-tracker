// CoreMIDI: each firmware port is a virtual source and destination ("Meadowlark USB"),
// plus an input port connected to the chosen sources and an output port that sends to the
// chosen destinations. Messages travel as MIDI 1.0 in Universal MIDI Packets (the API
// since macOS 11).

#include "MidiBackend.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <cstdio>
#include <mutex>

namespace midi
{
namespace
{
std::string toString(CFStringRef text)
{
    if (!text)
        return {};
    char buffer[256];
    if (CFStringGetCString(text, buffer, sizeof(buffer), kCFStringEncodingUTF8))
        return buffer;
    return {};
}

std::string endpointName(MIDIEndpointRef endpoint)
{
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) != noErr || !name)
        return "?";
    std::string result = toString(name);
    CFRelease(name);
    return result;
}

// Words in a Universal MIDI Packet, by message type (its top 4 bits).
int umpWords(uint32_t word)
{
    static const uint8_t kWords[16] = { 1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4 };
    return kWords[word >> 28];
}

class CoreMidiBackend : public MidiBackend
{
  public:
    ~CoreMidiBackend() override { stop(); }
    const char* name() const override { return "coremidi"; }

    bool start(Receive receive) override
    {
        this->receive = std::move(receive);
        OSStatus status = MIDIClientCreateWithBlock(CFSTR("Meadowlark"), &client, nil);
        if (status != noErr)
        {
            std::fprintf(stderr, "meadowlark: coremidi: cannot create a client (%d)\n", static_cast<int>(status));
            return false;
        }
        for (int port = 0; port < kPortCount; port++)
        {
            CFStringRef title = port == kPortJack ? CFSTR("Meadowlark Jack") : CFSTR("Meadowlark USB");
            MIDIReceiveBlock block = ^(const MIDIEventList* list, void*) {
              deliver(port, list);
            };
            status = MIDISourceCreateWithProtocol(client, title, kMIDIProtocol_1_0, &ports[port].source);
            if (status == noErr)
                status = MIDIDestinationCreateWithProtocol(client, title, kMIDIProtocol_1_0, &ports[port].destination, block);
            if (status == noErr)
                status = MIDIInputPortCreateWithProtocol(client, title, kMIDIProtocol_1_0, &ports[port].input, block);
            if (status == noErr)
                status = MIDIOutputPortCreate(client, title, &ports[port].output);
            if (status != noErr)
            {
                std::fprintf(stderr, "meadowlark: coremidi: cannot create the %s port (%d)\n", portName(port), static_cast<int>(status));
                stop();
                return false;
            }
        }
        return true;
    }

    void stop() override
    {
        if (!client)
            return;
        // Disposing of the client disposes of its ports and virtual endpoints.
        MIDIClientDispose(client);
        client = 0;
        for (Ports& port : ports)
            port = Ports{};
    }

    std::vector<Endpoint> sources() override
    {
        std::vector<Endpoint> result;
        for (ItemCount i = 0, n = MIDIGetNumberOfSources(); i < n; i++)
            result.push_back(describe(MIDIGetSource(i), true));
        return result;
    }

    std::vector<Endpoint> destinations() override
    {
        std::vector<Endpoint> result;
        for (ItemCount i = 0, n = MIDIGetNumberOfDestinations(); i < n; i++)
            result.push_back(describe(MIDIGetDestination(i), false));
        return result;
    }

    bool connectInput(int port, const Endpoint& source) override
    {
        return MIDIPortConnectSource(ports[port].input, static_cast<MIDIEndpointRef>(source.id), nullptr) == noErr;
    }

    bool connectOutput(int port, const Endpoint& destination) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        ports[port].targets.push_back(static_cast<MIDIEndpointRef>(destination.id));
        return true;
    }

    void send(int port, const Message& message) override
    {
        const size_t size = messageSize(message.status);
        if (size == 0 || !client)
            return;
        // MIDI 1.0 in a UMP: type 1 for system messages, 2 for channel voice, group 0.
        uint32_t word = (message.status >= 0xF0 ? 0x10000000u : 0x20000000u) | (uint32_t(message.status) << 16);
        if (size > 1)
            word |= uint32_t(message.data1) << 8;
        if (size > 2)
            word |= message.data2;

        MIDIEventList list;
        MIDIEventPacket* packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);
        packet = MIDIEventListAdd(&list, sizeof(list), packet, 0, 1, &word);
        if (!packet)
            return;

        MIDIReceivedEventList(ports[port].source, &list);
        std::lock_guard<std::mutex> lock(mutex);
        for (MIDIEndpointRef target : ports[port].targets)
            MIDISendEventList(ports[port].output, target, &list);
    }

  private:
    struct Ports
    {
        MIDIEndpointRef source = 0;
        MIDIEndpointRef destination = 0;
        MIDIPortRef input = 0;
        MIDIPortRef output = 0;
        std::vector<MIDIEndpointRef> targets;
    };

    Endpoint describe(MIDIEndpointRef endpoint, bool isSource)
    {
        Endpoint result;
        result.id = endpoint;
        result.name = endpointName(endpoint);
        for (const Ports& port : ports)
            result.own = result.own || endpoint == (isSource ? port.source : port.destination);
        // Endpoints of a device (driver-owned) have an entity; other programs' virtual ones don't.
        MIDIEntityRef entity = 0;
        result.hardware = !result.own && MIDIEndpointGetEntity(endpoint, &entity) == noErr && entity != 0;
        return result;
    }

    void deliver(int port, const MIDIEventList* list)
    {
        const MIDIEventPacket* packet = &list->packet[0];
        for (UInt32 p = 0; p < list->numPackets; p++)
        {
            for (UInt32 i = 0; i < packet->wordCount; i += umpWords(packet->words[i]))
            {
                const uint32_t word = packet->words[i];
                const uint32_t type = word >> 28;
                if (type != 1 && type != 2)
                    continue; // utility, SysEx, MIDI 2.0
                Message message;
                message.status = static_cast<uint8_t>(word >> 16);
                message.data1 = static_cast<uint8_t>(word >> 8) & 0x7F;
                message.data2 = static_cast<uint8_t>(word) & 0x7F;
                if (messageSize(message.status) != 0)
                    receive(port, message);
            }
            packet = MIDIEventPacketNext(packet);
        }
    }

    Receive receive;
    MIDIClientRef client = 0;
    Ports ports[kPortCount];
    std::mutex mutex;
};
} // namespace

std::unique_ptr<MidiBackend> createMidiBackend()
{
    return std::make_unique<CoreMidiBackend>();
}
} // namespace midi
