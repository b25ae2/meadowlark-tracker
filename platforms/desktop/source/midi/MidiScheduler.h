#pragma once

// Sends the firmware's outgoing MIDI when the audio rendered alongside it is heard.
//
// Blocks render ahead of playback by the audio buffer, so a note or clock sent while a
// block renders belongs to that block's playback time (the timebase), plus its offset into
// the block. Messages from the main loop get the next block's time, which is when sound
// they trigger plays too. A thread sends each message when it is due, in the order sent
// (a message never overtakes an earlier one, so a later due time holds back the next).
//
// Without a timebase (no sound device), messages go out at once.

#include "midi/MidiPorts.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace midi
{
class MidiScheduler : public Sink
{
  public:
    using Clock = std::chrono::steady_clock;
    using Timebase = std::function<std::optional<Clock::time_point>()>;
    // Called on the scheduler's thread.
    using Output = std::function<void(int port, const Message& message)>;

    static constexpr size_t kCapacity = 4096;

    ~MidiScheduler() override { stop(); }

    void start(Output output, Timebase timebase, Clock::duration offset);
    // Sends whatever is still queued at once, then stops.
    void stop();

    void send(int port, const Message& message, uint32_t delayUs) override;

    // Statistics, to read after stop().
    uint64_t messagesSent() const { return sentCount; }
    uint64_t messagesDropped() const { return droppedCount; }
    // How late the latest messages went out, worst case, in microseconds.
    int64_t worstLatenessUs() const { return worstLateness; }

  private:
    struct Event
    {
        Clock::time_point due;
        int port;
        Message message;
    };

    void run();

    Output output;
    Timebase timebase;
    Clock::duration offset{};

    std::mutex mutex;
    std::condition_variable wake;
    Event ring[kCapacity];
    size_t head = 0;
    size_t count = 0;
    Clock::time_point lastDue{};
    bool running = false;
    std::thread thread;

    uint64_t sentCount = 0;
    uint64_t droppedCount = 0;
    int64_t worstLateness = 0;
};
} // namespace midi
