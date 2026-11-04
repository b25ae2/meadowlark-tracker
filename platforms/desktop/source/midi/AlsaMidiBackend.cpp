// ALSA sequencer: one client "Meadowlark" with a duplex port per firmware port ("USB",
// "Jack"). PipeWire bridges the sequencer, so this covers USB MIDI devices, other programs
// and PipeWire's MIDI alike. The client is shared by the receive thread, the scheduler's
// sends and the main loop's connections; alsa-lib wants one caller at a time, so all three
// take the mutex, and the receive thread waits in poll() without it.

#include "MidiBackend.h"

#include <alsa/asoundlib.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace midi
{
namespace
{
constexpr const char* kPortTitles[kPortCount] = { "USB", "Jack" };

class AlsaMidiBackend : public MidiBackend
{
  public:
    ~AlsaMidiBackend() override { stop(); }
    const char* name() const override { return "alsa"; }

    bool start(Receive receive) override
    {
        this->receive = std::move(receive);
        int error = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
        if (error < 0)
        {
            std::fprintf(stderr, "meadowlark: alsa: cannot open the sequencer: %s\n", snd_strerror(error));
            seq = nullptr;
            return false;
        }
        snd_seq_set_client_name(seq, "Meadowlark");
        clientId = snd_seq_client_id(seq);

        for (int port = 0; port < kPortCount; port++)
        {
            ports[port] = snd_seq_create_simple_port(seq,
                                                     kPortTitles[port],
                                                     SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
                                                       SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE |
                                                       SND_SEQ_PORT_CAP_DUPLEX,
                                                     SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
            if (ports[port] < 0)
            {
                std::fprintf(stderr, "meadowlark: alsa: cannot create the %s port: %s\n", portName(port), snd_strerror(ports[port]));
                stop();
                return false;
            }
        }

        if (snd_midi_event_new(16, &encoder) < 0 || snd_midi_event_new(16, &decoder) < 0 || pipe(wakePipe) != 0)
        {
            std::fprintf(stderr, "meadowlark: alsa: out of resources\n");
            stop();
            return false;
        }
        snd_midi_event_no_status(decoder, 1); // every message with its status byte

        running = true;
        thread = std::thread([this] { run(); });
        return true;
    }

    void stop() override
    {
        if (thread.joinable())
        {
            running = false;
            const char byte = 0;
            (void)!write(wakePipe[1], &byte, 1);
            thread.join();
        }
        for (int& fd : wakePipe)
        {
            if (fd >= 0)
                close(fd);
            fd = -1;
        }
        if (encoder)
            snd_midi_event_free(encoder);
        if (decoder)
            snd_midi_event_free(decoder);
        encoder = decoder = nullptr;
        if (seq)
            snd_seq_close(seq);
        seq = nullptr;
    }

    std::vector<Endpoint> sources() override { return list(SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ); }
    std::vector<Endpoint> destinations() override { return list(SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE); }

    bool connectInput(int port, const Endpoint& source) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int error = snd_seq_connect_from(seq, ports[port], int(source.id >> 8), int(source.id & 0xFF));
        return error == 0 || error == -EBUSY; // EBUSY: already connected
    }

    bool connectOutput(int port, const Endpoint& destination) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int error = snd_seq_connect_to(seq, ports[port], int(destination.id >> 8), int(destination.id & 0xFF));
        return error == 0 || error == -EBUSY;
    }

    void send(int port, const Message& message) override
    {
        const size_t size = messageSize(message.status);
        if (size == 0 || !seq)
            return;
        const unsigned char bytes[3] = { message.status, message.data1, message.data2 };

        std::lock_guard<std::mutex> lock(mutex);
        snd_seq_event_t event;
        snd_seq_ev_clear(&event);
        snd_midi_event_reset_encode(encoder);
        if (snd_midi_event_encode(encoder, bytes, static_cast<long>(size), &event) <= 0 || event.type == SND_SEQ_EVENT_NONE)
            return;
        snd_seq_ev_set_source(&event, ports[port]);
        snd_seq_ev_set_subs(&event);
        snd_seq_ev_set_direct(&event);
        snd_seq_event_output_direct(seq, &event);
    }

  private:
    std::vector<Endpoint> list(unsigned int capabilities)
    {
        std::vector<Endpoint> result;
        std::lock_guard<std::mutex> lock(mutex);
        if (!seq)
            return result;

        snd_seq_client_info_t* client;
        snd_seq_port_info_t* port;
        snd_seq_client_info_alloca(&client);
        snd_seq_port_info_alloca(&port);
        snd_seq_client_info_set_client(client, -1);
        while (snd_seq_query_next_client(seq, client) >= 0)
        {
            const int id = snd_seq_client_info_get_client(client);
            if (id == SND_SEQ_CLIENT_SYSTEM)
                continue;
            snd_seq_port_info_set_client(port, id);
            snd_seq_port_info_set_port(port, -1);
            while (snd_seq_query_next_port(seq, port) >= 0)
            {
                const unsigned int caps = snd_seq_port_info_get_capability(port);
                if ((caps & capabilities) != capabilities || (caps & SND_SEQ_PORT_CAP_NO_EXPORT))
                    continue;
                Endpoint endpoint;
                endpoint.id = (static_cast<uint64_t>(id) << 8) | static_cast<uint64_t>(snd_seq_port_info_get_port(port));
                endpoint.name = std::string(snd_seq_client_info_get_name(client)) + ":" + snd_seq_port_info_get_name(port);
                endpoint.own = id == clientId;
                // USB and other device drivers mark their ports; Midi Through and programs don't.
                endpoint.hardware = (snd_seq_port_info_get_type(port) & SND_SEQ_PORT_TYPE_HARDWARE) != 0;
                result.push_back(endpoint);
            }
        }
        return result;
    }

    void run()
    {
        std::vector<pollfd> fds;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const int count = snd_seq_poll_descriptors_count(seq, POLLIN);
            fds.resize(static_cast<size_t>(count) + 1);
            snd_seq_poll_descriptors(seq, fds.data(), static_cast<unsigned int>(count), POLLIN);
        }
        fds.back() = pollfd{ wakePipe[0], POLLIN, 0 };

        while (running)
        {
            if (poll(fds.data(), fds.size(), -1) < 0 && errno != EINTR)
                break;
            if (!running)
                break;

            std::lock_guard<std::mutex> lock(mutex);
            snd_seq_event_t* event = nullptr;
            while (snd_seq_event_input(seq, &event) >= 0 && event)
            {
                const int port = event->dest.port == ports[kPortJack] ? kPortJack : kPortUsb;
                unsigned char bytes[16];
                const long size = snd_midi_event_decode(decoder, bytes, sizeof(bytes), event);
                if (size > 0 && messageSize(bytes[0]) == static_cast<size_t>(size))
                {
                    Message message;
                    message.status = bytes[0];
                    message.data1 = size > 1 ? bytes[1] : 0;
                    message.data2 = size > 2 ? bytes[2] : 0;
                    receive(port, message);
                }
                event = nullptr;
            }
        }
    }

    Receive receive;
    snd_seq_t* seq = nullptr;
    int clientId = -1;
    int ports[kPortCount] = { -1, -1 };
    snd_midi_event_t* encoder = nullptr;
    snd_midi_event_t* decoder = nullptr;
    int wakePipe[2] = { -1, -1 };
    std::mutex mutex;
    std::thread thread;
    std::atomic<bool> running{ false };
};
} // namespace

std::unique_ptr<MidiBackend> createMidiBackend()
{
    return std::make_unique<AlsaMidiBackend>();
}
} // namespace midi
