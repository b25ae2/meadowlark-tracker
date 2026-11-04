#pragma once

// Runs the engine on its own thread and hands finished audio to an output backend.
//
// The render thread stands in for the firmware's audio interrupt: it renders 128-frame
// blocks (the caller's RenderBlock takes AudioLock around core) into a ring buffer and keeps
// it filled to bufferFrames. Backends pull frames with read() from their real-time callback,
// which never blocks or takes a lock, so the main loop holding AudioLock for a while costs
// buffer headroom rather than stalling the sound server.
//
// Latency is bufferFrames plus whatever the backend adds.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class AudioRenderer
{
  public:
    static constexpr int kBlockFrames = 128;
    static constexpr int kSampleRate = 44100;

    using RenderBlock = std::function<void(int16_t* left, int16_t* right, int frames)>;

    ~AudioRenderer() { stop(); }

    // Fills the buffer before returning, so a backend started next has audio at once.
    void start(RenderBlock render, int bufferFrames);
    void stop();
    bool isRunning() const { return thread.joinable(); }
    int bufferFrames() const { return static_cast<int>(target); }

    // Consumer side, any thread, real-time safe: copies interleaved stereo frames and fills
    // whatever isn't ready with silence (an underrun).
    void read(int16_t* interleaved, size_t frames);

    uint64_t underruns() const { return underrunCount.load(std::memory_order_relaxed); }
    uint64_t blocksRendered() const { return blockCount.load(std::memory_order_relaxed); }

  private:
    void run();
    size_t filled() const;
    void renderOneBlock();

    RenderBlock renderBlock;
    std::vector<int16_t> ring; // interleaved stereo
    size_t mask = 0;          // ring size in frames - 1
    size_t target = 0;
    std::atomic<size_t> writePos{ 0 }; // frames, only grows
    std::atomic<size_t> readPos{ 0 };

    std::thread thread;
    std::atomic<bool> running{ false };
    std::atomic<bool> waiting{ false };
    std::mutex wakeMutex;
    std::condition_variable wake;

    std::atomic<uint64_t> underrunCount{ 0 };
    std::atomic<uint64_t> blockCount{ 0 };
    int16_t left[kBlockFrames] = {};
    int16_t right[kBlockFrames] = {};
};

// An audio device or sound server connection that pulls from an AudioRenderer.
class AudioOutput
{
  public:
    virtual ~AudioOutput() = default;
    virtual const char* name() const = 0;
    // The renderer must be running. Returns false, with a message on stderr, if the device
    // can't be opened.
    virtual bool start(AudioRenderer& renderer) = 0;
    virtual void stop() = 0;
};
