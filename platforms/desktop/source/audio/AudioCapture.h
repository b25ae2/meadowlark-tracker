#pragma once

// Audio input: a capture backend writes into AudioCaptureBuffer from its real-time callback,
// and the render thread takes one block per engine block, as the codec's I2S input does on
// hardware.
//
// Input and output devices run on separate clocks, so the buffer keeps its own latency in
// check: reading starts once a capture callback's worth plus a block has arrived (priming),
// a shortfall gives a block of silence and primes again, and when the fill grows past twice
// the largest capture callback plus four blocks, the oldest frames are dropped.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class AudioCaptureBuffer
{
  public:
    static constexpr size_t kCapacityFrames = 8192;

    AudioCaptureBuffer() : ring(kCapacityFrames * 2, 0) {}

    // Producer (capture callback): interleaved stereo. Frames that don't fit are dropped.
    void write(const int16_t* interleaved, size_t frames);
    // Consumer (render thread): interleaved stereo, silence when not enough has arrived.
    void readBlock(int16_t* interleaved, size_t frames);

    uint64_t framesCaptured() const { return captured.load(std::memory_order_relaxed); }
    uint64_t underruns() const { return underrunCount.load(std::memory_order_relaxed); }
    uint64_t framesDropped() const { return dropped.load(std::memory_order_relaxed); }

  private:
    std::vector<int16_t> ring;
    std::atomic<size_t> writePos{ 0 };
    std::atomic<size_t> readPos{ 0 };
    std::atomic<size_t> largestWrite{ 0 };
    bool primed = false; // consumer only

    std::atomic<uint64_t> captured{ 0 };
    std::atomic<uint64_t> underrunCount{ 0 };
    std::atomic<uint64_t> dropped{ 0 };
};

// A capture device or sound server connection that writes into an AudioCaptureBuffer:
// 44.1 kHz, stereo, signed 16-bit.
class AudioInput
{
  public:
    virtual ~AudioInput() = default;
    virtual const char* name() const = 0;
    // Returns false, with a message on stderr, if capture can't start.
    virtual bool start(AudioCaptureBuffer& buffer) = 0;
    virtual void stop() = 0;
};
