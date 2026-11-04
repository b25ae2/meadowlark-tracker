#include "MidiScheduler.h"

#include <algorithm>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

namespace midi
{
namespace
{
constexpr std::chrono::microseconds kFinalSleep(2000);
}

void MidiScheduler::start(Output output, Timebase timebase, Clock::duration offset)
{
    stop();
    this->output = std::move(output);
    this->timebase = std::move(timebase);
    this->offset = offset;
    head = 0;
    count = 0;
    lastDue = Clock::time_point{};
    sentCount = 0;
    droppedCount = 0;
    worstLateness = 0;
    running = true;
    thread = std::thread([this] { run(); });
}

void MidiScheduler::stop()
{
    if (!thread.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        running = false;
    }
    wake.notify_one();
    thread.join();
}

void MidiScheduler::send(int port, const Message& message, uint32_t delayUs)
{
    const Clock::time_point now = Clock::now();
    std::optional<Clock::time_point> base = timebase ? timebase() : std::nullopt;
    Clock::time_point due = base ? *base + std::chrono::microseconds(delayUs) + offset : now;

    bool wasEmpty = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running)
            return;
        if (count == kCapacity)
        {
            droppedCount++;
            return;
        }
        due = std::max(due, lastDue);
        lastDue = due;
        ring[(head + count) % kCapacity] = Event{ due, port, message };
        wasEmpty = count == 0;
        count++;
    }
    if (wasEmpty)
        wake.notify_one();
}

void MidiScheduler::run()
{
#ifdef __APPLE__
    // Ordinary threads' timers get several milliseconds of slack; a time-constraint thread
    // (what audio threads are) wakes on time. It asks for little: 0.1 ms of work per 1 ms.
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    const auto ticks = [&](double ms) { return static_cast<uint32_t>(ms * 1e6 * timebase.denom / timebase.numer); };
    thread_time_constraint_policy_data_t policy;
    policy.period = ticks(1.0);
    policy.computation = ticks(0.1);
    policy.constraint = ticks(1.0);
    policy.preemptible = 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy),
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#endif
    std::unique_lock<std::mutex> lock(mutex);
    for (;;)
    {
        if (count == 0)
        {
            if (!running)
                return;
            wake.wait(lock, [this] { return count > 0 || !running; });
            continue;
        }

        const Event event = ring[head];
        const Clock::time_point now = Clock::now();
        if (running && now < event.due)
        {
            // A new message can only be due later, so only stopping cuts this short. Timed
            // waits on a condition variable get slack proportional to their length (macOS
            // coalesces timers), so the last stretch is a plain sleep, which is precise.
            if (event.due - now > kFinalSleep)
                wake.wait_until(lock, event.due - kFinalSleep, [this] { return !running; });
            else
            {
                lock.unlock();
                std::this_thread::sleep_until(event.due);
                lock.lock();
            }
            continue;
        }

        head = (head + 1) % kCapacity;
        count--;
        lock.unlock();
        const int64_t lateUs =
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - event.due).count();
        output(event.port, event.message);
        lock.lock();
        sentCount++;
        if (running)
            worstLateness = std::max(worstLateness, lateUs);
    }
}
} // namespace midi
