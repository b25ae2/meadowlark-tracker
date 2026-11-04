// MIDI checks: the firmware ports' encoding and dispatch (as UsbMidiImpl and the Arduino MIDI
// library do it on the Mini), MIDI clock offsets, the scheduler's timing, and midi.toml.
//
// --loopback also starts the system backend (ALSA sequencer or CoreMIDI), connects the usb
// port's output to the jack port's input through it, and checks that timed messages arrive
// complete, in order and on time.
//
//   midi_test <scratch dir> [--loopback]

#include "PlatformDesktop.h"
#include "midi/MidiBackend.h"
#include "midi/MidiConfig.h"
#include "midi/MidiPorts.h"
#include "midi/MidiScheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace
{
int failures = 0;

#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                   \
        }                                                                 \
    } while (0)

using Clock = std::chrono::steady_clock;

struct Sent
{
    int port;
    midi::Message message;
    uint32_t delayUs;
};

class FakeSink : public midi::Sink
{
  public:
    std::vector<Sent> sent;
    void send(int port, const midi::Message& message, uint32_t delayUs) override
    {
        sent.push_back(Sent{ port, message, delayUs });
    }
};

bool sentIs(const Sent& sent, int port, uint8_t status, uint8_t data1, uint8_t data2)
{
    return sent.port == port && sent.message.status == status && sent.message.data1 == data1 &&
           sent.message.data2 == data2;
}

midi::Message message(uint8_t status, uint8_t data1 = 0, uint8_t data2 = 0)
{
    midi::Message result;
    result.status = status;
    result.data1 = data1;
    result.data2 = data2;
    return result;
}

void checkMessageSizes()
{
    CHECK(midi::messageSize(0x90) == 3);
    CHECK(midi::messageSize(0x8F) == 3);
    CHECK(midi::messageSize(0xC3) == 2);
    CHECK(midi::messageSize(0xD0) == 2);
    CHECK(midi::messageSize(0xE0) == 3);
    CHECK(midi::messageSize(0xF2) == 3);
    CHECK(midi::messageSize(0xF8) == 1);
    CHECK(midi::messageSize(0xFC) == 1);
    CHECK(midi::messageSize(0xF0) == 0);
    CHECK(midi::messageSize(0xF7) == 0);
    CHECK(midi::messageSize(0x40) == 0);
}

// --- sending ---------------------------------------------------------------------------
void checkSending()
{
    midi::DesktopUsbMidi usb;
    midi::DesktopUartMidi jack;
    FakeSink sink;

    // No sink: dropped quietly.
    usb.sendNoteOn(60, 100, 1);

    usb.io().setSink(&sink);
    jack.io().setSink(&sink);
    usb.sendNoteOn(60, 100, 1);
    usb.sendNoteOff(60, 0, 16);
    usb.sendControlChange(7, 127, 2);
    usb.sendProgramChange(5, 3);
    usb.sendAfterTouch(90, 4);
    usb.sendPolyPressure(61, 80, 5);
    usb.sendPitchBend(-8192, 1);
    usb.sendPitchBend(0, 1);
    usb.sendPitchBend(8191, 1);
    usb.sendPitchBend(20000, 1); // clamped
    usb.sendRealTime(Midi::kClock);
    usb.sendRealTime(Midi::kTuneRequest); // not a real-time message: not sent
    jack.sendNoteOn(64, 1, 10);
    jack.sendRealTime(Midi::kStart);
    jack.sendRealTime(Midi::kStop);

    CHECK(sink.sent.size() == 14);
    if (sink.sent.size() != 14)
        return;
    CHECK(sentIs(sink.sent[0], midi::kPortUsb, 0x90, 60, 100));
    CHECK(sentIs(sink.sent[1], midi::kPortUsb, 0x8F, 60, 0));
    CHECK(sentIs(sink.sent[2], midi::kPortUsb, 0xB1, 7, 127));
    CHECK(sentIs(sink.sent[3], midi::kPortUsb, 0xC2, 5, 0));
    CHECK(sentIs(sink.sent[4], midi::kPortUsb, 0xD3, 90, 0));
    CHECK(sentIs(sink.sent[5], midi::kPortUsb, 0xA4, 61, 80));
    CHECK(sentIs(sink.sent[6], midi::kPortUsb, 0xE0, 0x00, 0x00));
    CHECK(sentIs(sink.sent[7], midi::kPortUsb, 0xE0, 0x00, 0x40));
    CHECK(sentIs(sink.sent[8], midi::kPortUsb, 0xE0, 0x7F, 0x7F));
    CHECK(sentIs(sink.sent[9], midi::kPortUsb, 0xE0, 0x7F, 0x7F));
    CHECK(sentIs(sink.sent[10], midi::kPortUsb, 0xF8, 0, 0));
    CHECK(sentIs(sink.sent[11], midi::kPortJack, 0x99, 64, 1));
    CHECK(sentIs(sink.sent[12], midi::kPortJack, 0xFA, 0, 0));
    CHECK(sentIs(sink.sent[13], midi::kPortJack, 0xFC, 0, 0));
    for (const Sent& sent : sink.sent)
        CHECK(sent.delayUs == 0);
}

// --- receiving -------------------------------------------------------------------------
std::vector<std::string> received;

void record(const char* what, int a, int b, int c)
{
    received.push_back(std::string(what) + " " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c));
}

void usbRealTime(uint8_t type) { record("rt", type, 0, 0); }
void usbNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) { record("on", channel, note, velocity); }
void usbNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) { record("off", channel, note, velocity); }
void usbSongPosition(uint16_t beats) { record("spp", beats, 0, 0); }
void usbControlChange(uint8_t channel, uint8_t number, uint8_t value) { record("cc", channel, number, value); }
void jackClock() { record("clock", 0, 0, 0); }
void jackStart() { record("start", 0, 0, 0); }
void jackStop() { record("stop", 0, 0, 0); }
void jackContinue() { record("continue", 0, 0, 0); }
void jackSongPosition(unsigned beats) { record("spp", static_cast<int>(beats), 0, 0); }
void jackNoteOnOverride(uint8_t channel, uint8_t note, uint8_t velocity) { record("on2", channel, note, velocity); }

// What midiUpdate() does for each port.
template <typename Port>
void poll(Port& port)
{
    while (port.isReadAvailable())
        port.read();
}

void checkReceiving()
{
    midi::DesktopUsbMidi usb;
    UsbMidi::MidiReceiveActions usbActions{ usbRealTime, usbNoteOn, usbNoteOff, usbSongPosition, usbControlChange };
    usb.setReceiveActions(&usbActions);

    received.clear();
    midi::InQueue& usbIn = usb.io().input();
    usbIn.push(message(0x90, 60, 100));
    usbIn.push(message(0x91, 61, 0)); // velocity 0 stays a note on, as on IMX
    usbIn.push(message(0x8F, 62, 64));
    usbIn.push(message(0xB2, 7, 99));
    usbIn.push(message(0xF2, 0x10, 0x02)); // 0x110
    usbIn.push(message(0xF8));
    usbIn.push(message(0xFA));
    usbIn.push(message(0xFE)); // active sensing: ignored
    usbIn.push(message(0xC0, 3)); // program change: ignored
    poll(usb);
    const std::vector<std::string> usbExpected = { "on 1 60 100", "on 2 61 0", "off 16 62 64", "cc 3 7 99",
                                                   "spp 272 0 0", "rt 248 0 0", "rt 250 0 0" };
    CHECK(received == usbExpected);

    midi::DesktopUartMidi jack;
    UartMidi::MidiReceiveActions jackActions{ usbNoteOn,    usbNoteOff,       jackClock,       jackStart,
                                              jackStop,     jackSongPosition, jackContinue,    usbControlChange };
    jack.setReceiveActions(&jackActions);
    received.clear();
    midi::InQueue& jackIn = jack.io().input();
    jackIn.push(message(0x90, 60, 100));
    jackIn.push(message(0x90, 60, 0)); // velocity 0 is a note off, as in the Arduino library
    jackIn.push(message(0xF8));
    jackIn.push(message(0xFA));
    jackIn.push(message(0xFB));
    jackIn.push(message(0xFC));
    jackIn.push(message(0xF2, 0x7F, 0x7F));
    poll(jack);
    jack.setReceiveNoteOnAction(jackNoteOnOverride);
    jackIn.push(message(0x95, 1, 2));
    poll(jack);
    const std::vector<std::string> jackExpected = { "on 1 60 100", "off 1 60 0", "clock 0 0 0", "start 0 0 0",
                                                    "continue 0 0 0", "stop 0 0 0", "spp 16383 0 0", "on2 6 1 2" };
    CHECK(received == jackExpected);

    // A full queue drops, and keeps the earlier messages.
    midi::InQueue queue;
    for (size_t i = 0; i < midi::InQueue::kCapacity + 3; i++)
        queue.push(message(0x90, static_cast<uint8_t>(i & 0x7F), 1));
    CHECK(queue.dropped() == 3);
    midi::Message first;
    CHECK(queue.tryPop(first) && first.data1 == 0);
}

// --- MIDI clock inside a block ---------------------------------------------------------
midi::DesktopUsbMidi* clockPort = nullptr;
void sendClockNow()
{
    clockPort->sendRealTime(Midi::kClock);
}

void checkClockOffset()
{
    DesktopTimer timer;
    midi::DesktopUsbMidi usb;
    FakeSink sink;
    usb.io().setSink(&sink);
    clockPort = &usb;

    timer.markAudioBlockStart();
    timer.scheduleMidiClockUs(1500, sendClockNow);
    usb.sendRealTime(Midi::kClock); // not scheduled: no delay
    CHECK(sink.sent.size() == 2);
    if (sink.sent.size() == 2)
    {
        // The delay counts from the block's start; a little time passed since.
        CHECK(sink.sent[0].delayUs >= 1500 && sink.sent[0].delayUs < 1500 + 20000);
        CHECK(sink.sent[1].delayUs == 0);
    }
    CHECK(DesktopTimer::midiDelayUs() == 0);
}

// --- scheduler -------------------------------------------------------------------------
struct Arrival
{
    int port;
    midi::Message message;
    Clock::time_point time;
};

int64_t microsBetween(Clock::time_point from, Clock::time_point to)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
}

void checkScheduler()
{
    std::mutex mutex;
    std::vector<Arrival> arrivals;
    auto output = [&](int port, const midi::Message& message) {
        std::lock_guard<std::mutex> lock(mutex);
        arrivals.push_back(Arrival{ port, message, Clock::now() });
    };

    const Clock::time_point base = Clock::now() + std::chrono::milliseconds(40);
    midi::MidiScheduler scheduler;
    scheduler.start(output, [&] { return std::optional<Clock::time_point>(base); }, std::chrono::milliseconds(2));
    scheduler.send(midi::kPortUsb, message(0x90, 1, 1), 0);
    scheduler.send(midi::kPortJack, message(0x90, 2, 1), 5000);
    scheduler.send(midi::kPortUsb, message(0x90, 3, 1), 1000); // earlier, but waits its turn
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    scheduler.stop();

    CHECK(arrivals.size() == 3);
    if (arrivals.size() == 3)
    {
        CHECK(arrivals[0].message.data1 == 1 && arrivals[0].port == midi::kPortUsb);
        CHECK(arrivals[1].message.data1 == 2 && arrivals[1].port == midi::kPortJack);
        CHECK(arrivals[2].message.data1 == 3);
        const int64_t first = microsBetween(base, arrivals[0].time);
        const int64_t second = microsBetween(base, arrivals[1].time);
        const int64_t third = microsBetween(base, arrivals[2].time);
        std::printf("scheduler: due +2000/+7000/+7000 us, sent %+lld/%+lld/%+lld us\n",
                    static_cast<long long>(first),
                    static_cast<long long>(second),
                    static_cast<long long>(third));
        CHECK(first >= 2000 && first < 2000 + 3000);
        CHECK(second >= 7000 && second < 7000 + 3000);
        CHECK(third >= 7000 && third < 7000 + 3000);
    }
    CHECK(scheduler.messagesSent() == 3);

    // No timebase yet (no sound device): at once.
    arrivals.clear();
    scheduler.start(output, [] { return std::optional<Clock::time_point>(); }, std::chrono::milliseconds(0));
    const Clock::time_point sentAt = Clock::now();
    scheduler.send(midi::kPortUsb, message(0xF8), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    scheduler.stop();
    CHECK(arrivals.size() == 1);
    if (arrivals.size() == 1)
        CHECK(microsBetween(sentAt, arrivals[0].time) < 3000);

    // Stopping sends what is left at once.
    arrivals.clear();
    scheduler.start(output, [] { return std::optional<Clock::time_point>(Clock::now() + std::chrono::seconds(10)); },
                    std::chrono::milliseconds(0));
    scheduler.send(midi::kPortUsb, message(0xFC), 0);
    scheduler.stop();
    CHECK(arrivals.size() == 1);
}

// --- midi.toml -------------------------------------------------------------------------
bool writeFile(const std::string& path, const char* text)
{
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file)
        return false;
    std::fputs(text, file);
    std::fclose(file);
    return true;
}

void checkConfig(const std::string& scratch)
{
    midi::MidiConfig config;
    CHECK(midi::loadMidiConfig(scratch + "/missing.toml", config));
    CHECK(config.enabled);
    CHECK(config.ports[midi::kPortUsb].inputs == std::vector<std::string>{ "*" });
    CHECK(config.ports[midi::kPortUsb].outputs.empty());
    CHECK(config.ports[midi::kPortJack].inputs.empty());
    CHECK(config.outputOffsetMs == 0.0);

    const std::string path = scratch + "/midi.toml";
    CHECK(writeFile(path,
                    "[jack]\n"
                    "outputs = [\"Synth\", \"Midi Through\"]\n"
                    "[timing]\n"
                    "output_offset_ms = -3.5\n"
                    "[frobnicate]\n"));
    CHECK(midi::loadMidiConfig(path, config));
    CHECK(config.ports[midi::kPortUsb].inputs == std::vector<std::string>{ "*" }); // default kept
    CHECK((config.ports[midi::kPortJack].outputs == std::vector<std::string>{ "Synth", "Midi Through" }));
    CHECK(config.outputOffsetMs == -3.5);

    CHECK(writeFile(path, "[usb\n"));
    CHECK(!midi::loadMidiConfig(path, config));
    CHECK(config.ports[midi::kPortUsb].inputs == std::vector<std::string>{ "*" });

    CHECK(midi::matchesPattern("*", "Launchkey:MIDI 1", true));
    CHECK(!midi::matchesPattern("*", "Midi Through:Midi Through Port-0", false));
    CHECK(midi::matchesPattern("midi through", "Midi Through:Midi Through Port-0", false));
    CHECK(midi::matchesPattern("LAUNCHKEY", "Launchkey Mini MK3 MIDI Port", true));
    CHECK(!midi::matchesPattern("", "anything", true));
    CHECK(!midi::matchesPattern("Synth", "Launchkey", true));
}

// --- loopback through the system -------------------------------------------------------
bool runLoopback()
{
    std::unique_ptr<midi::MidiBackend> backend = midi::createMidiBackend();
    std::mutex mutex;
    std::vector<Arrival> arrivals;
    if (!backend->start([&](int port, const midi::Message& message) {
            std::lock_guard<std::mutex> lock(mutex);
            arrivals.push_back(Arrival{ port, message, Clock::now() });
        }))
        return false;

    // usb's output into jack's input: CoreMIDI calls it "Meadowlark Jack", ALSA "Meadowlark:Jack".
    midi::MidiConfig config;
    for (midi::PortConfig& port : config.ports)
        port = midi::PortConfig{};
    config.ports[midi::kPortUsb].outputs = { "Meadowlark Jack", "Meadowlark:Jack" };
    midi::MidiConnector connector(*backend, config);
    connector.update();
    midi::printMidiPorts(*backend, config);

    midi::MidiScheduler scheduler;
    const Clock::time_point base = Clock::now() + std::chrono::milliseconds(50);
    scheduler.start([&](int port, const midi::Message& message) { backend->send(port, message); },
                    [&] { return std::optional<Clock::time_point>(base); },
                    std::chrono::milliseconds(0));

    std::vector<midi::Message> expected;
    std::vector<uint32_t> delays;
    const midi::Message kinds[] = { message(0x90, 60, 100), message(0x80, 60, 0),   message(0xB0, 1, 64),
                                    message(0xC5, 7),       message(0xE3, 0, 0x40), message(0xF8),
                                    message(0xFA),          message(0xFC),          message(0xF2, 5, 1),
                                    message(0xD2, 90),      message(0xAF, 10, 20) };
    for (int i = 0; i < 100; i++)
    {
        expected.push_back(kinds[i % (sizeof(kinds) / sizeof(kinds[0]))]);
        delays.push_back(static_cast<uint32_t>(i) * 2000);
        scheduler.send(midi::kPortUsb, expected.back(), delays.back());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50 + 200 + 100));
    scheduler.stop();
    backend->stop();

    std::lock_guard<std::mutex> lock(mutex);
    CHECK(arrivals.size() == expected.size());
    int64_t worst = 0;
    int64_t earliest = 0;
    for (size_t i = 0; i < arrivals.size() && i < expected.size(); i++)
    {
        const Arrival& arrival = arrivals[i];
        CHECK(arrival.port == midi::kPortJack);
        CHECK(arrival.message.status == expected[i].status);
        if (midi::messageSize(expected[i].status) > 1)
            CHECK(arrival.message.data1 == expected[i].data1);
        if (midi::messageSize(expected[i].status) > 2)
            CHECK(arrival.message.data2 == expected[i].data2);
        const int64_t late = microsBetween(base + std::chrono::microseconds(delays[i]), arrival.time);
        worst = std::max(worst, late);
        earliest = std::min(earliest, late);
    }
    std::printf("loopback %s: %zu of %zu messages, %lld..%lld us after their time\n",
                backend->name(),
                arrivals.size(),
                expected.size(),
                static_cast<long long>(earliest),
                static_cast<long long>(worst));
    CHECK(earliest >= 0);
    CHECK(worst < 3000);
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: midi_test <scratch dir> [--loopback]\n");
        return 2;
    }
    const std::string scratch = argv[1];
    mkdir(scratch.c_str(), 0755);
    const bool loopback = argc > 2 && std::strcmp(argv[2], "--loopback") == 0;

    checkMessageSizes();
    checkSending();
    checkReceiving();
    checkClockOffset();
    checkScheduler();
    checkConfig(scratch);
    if (loopback && !runLoopback())
    {
        std::printf("FAIL: no system MIDI\n");
        failures++;
    }

    if (failures)
    {
        std::printf("%d MIDI check(s) failed\n", failures);
        return 1;
    }
    std::printf("all MIDI checks passed\n");
    return 0;
}
