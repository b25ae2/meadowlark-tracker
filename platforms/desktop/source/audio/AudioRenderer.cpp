#include "AudioRenderer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

void AudioRenderer::start(RenderBlock render, int bufferFrames)
{
    stop();
    renderBlock = std::move(render);

    // Whole blocks, at least two.
    const size_t blocks = std::max<size_t>(2, (static_cast<size_t>(std::max(bufferFrames, 0)) + kBlockFrames - 1) / kBlockFrames);
    target = blocks * kBlockFrames;
    size_t capacity = 1;
    while (capacity < target)
        capacity <<= 1;
    ring.assign(capacity * 2, 0);
    mask = capacity - 1;
    writePos = 0;
    readPos = 0;
    underrunCount = 0;
    blockCount = 0;

    while (filled() + kBlockFrames <= target)
        renderOneBlock();

    running = true;
    thread = std::thread([this] { run(); });
}

void AudioRenderer::stop()
{
    if (!thread.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(wakeMutex);
        running = false;
    }
    wake.notify_one();
    thread.join();
}

size_t AudioRenderer::filled() const
{
    return writePos.load(std::memory_order_acquire) - readPos.load(std::memory_order_acquire);
}

void AudioRenderer::renderOneBlock()
{
    renderBlock(left, right, kBlockFrames);

    const size_t start = writePos.load(std::memory_order_relaxed);
    for (int i = 0; i < kBlockFrames; i++)
    {
        const size_t frame = (start + static_cast<size_t>(i)) & mask;
        ring[frame * 2] = left[i];
        ring[frame * 2 + 1] = right[i];
    }
    writePos.store(start + kBlockFrames, std::memory_order_release);
    blockCount.fetch_add(1, std::memory_order_relaxed);
}

void AudioRenderer::run()
{
    while (running)
    {
        if (filled() + kBlockFrames <= target)
        {
            renderOneBlock();
            continue;
        }

        // Full: sleep until the consumer has taken a block. The timeout covers a wake-up
        // that races with going to sleep; the buffer has room for far more than that.
        std::unique_lock<std::mutex> lock(wakeMutex);
        waiting = true;
        wake.wait_for(lock, std::chrono::milliseconds(2), [this] {
            return !running || filled() + kBlockFrames <= target;
        });
        waiting = false;
    }
}

void AudioRenderer::read(int16_t* interleaved, size_t frames)
{
    const size_t start = readPos.load(std::memory_order_relaxed);
    const size_t available = writePos.load(std::memory_order_acquire) - start;
    const size_t count = std::min(available, frames);

    for (size_t i = 0; i < count; i++)
    {
        const size_t frame = (start + i) & mask;
        interleaved[i * 2] = ring[frame * 2];
        interleaved[i * 2 + 1] = ring[frame * 2 + 1];
    }
    if (count < frames)
    {
        std::memset(interleaved + count * 2, 0, (frames - count) * 2 * sizeof(int16_t));
        underrunCount.fetch_add(1, std::memory_order_relaxed);
    }
    readPos.store(start + count, std::memory_order_release);

    if (waiting.load(std::memory_order_relaxed))
        wake.notify_one();
}
