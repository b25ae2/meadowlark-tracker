#include "AudioCapture.h"

#include <algorithm>
#include <cstring>

namespace
{
constexpr size_t kMask = AudioCaptureBuffer::kCapacityFrames - 1;
}

void AudioCaptureBuffer::write(const int16_t* interleaved, size_t frames)
{
    if (frames > largestWrite.load(std::memory_order_relaxed))
        largestWrite.store(frames, std::memory_order_relaxed);

    const size_t start = writePos.load(std::memory_order_relaxed);
    const size_t used = start - readPos.load(std::memory_order_acquire);
    const size_t count = std::min(frames, kCapacityFrames - used);
    for (size_t i = 0; i < count; i++)
    {
        const size_t frame = (start + i) & kMask;
        ring[frame * 2] = interleaved[i * 2];
        ring[frame * 2 + 1] = interleaved[i * 2 + 1];
    }
    writePos.store(start + count, std::memory_order_release);
    captured.fetch_add(frames, std::memory_order_relaxed);
    if (count < frames)
        dropped.fetch_add(frames - count, std::memory_order_relaxed);
}

void AudioCaptureBuffer::readBlock(int16_t* interleaved, size_t frames)
{
    size_t start = readPos.load(std::memory_order_relaxed);
    size_t available = writePos.load(std::memory_order_acquire) - start;
    const size_t jitter = largestWrite.load(std::memory_order_relaxed);

    if (!primed && available >= jitter + frames && available >= frames)
        primed = true;
    if (!primed || available < frames)
    {
        if (primed)
            underrunCount.fetch_add(1, std::memory_order_relaxed);
        primed = false;
        std::memset(interleaved, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    // Keep latency bounded when the input clock runs ahead of the output's.
    const size_t limit = 2 * jitter + 4 * frames;
    if (available > limit)
    {
        const size_t excess = available - limit;
        start += excess;
        available -= excess;
        dropped.fetch_add(excess, std::memory_order_relaxed);
    }

    for (size_t i = 0; i < frames; i++)
    {
        const size_t frame = (start + i) & kMask;
        interleaved[i * 2] = ring[frame * 2];
        interleaved[i * 2 + 1] = ring[frame * 2 + 1];
    }
    readPos.store(start + frames, std::memory_order_release);
}
