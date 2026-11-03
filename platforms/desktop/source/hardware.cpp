// Desktop versions of the free functions the IMX platform provides in hardware.cpp.

#include <atomic>
#include <cstdint>

namespace
{
// Muted at startup, like the hardware codec output, until the UI unmutes it.
std::atomic<bool> audioOutMuted{ true };
} // namespace

// TODO(phase 3): apply this in the audio output path.
void muteAudioOutByKey()
{
    audioOutMuted = true;
}

void unmuteAudioOutByKey()
{
    audioOutMuted = false;
}

uint8_t isAudioOutMuted()
{
    return audioOutMuted ? 1 : 0;
}
