#include "AudioLock.h"

#include <mutex>

namespace
{
std::mutex audioMutex;
thread_local bool held = false;
} // namespace

namespace AudioLock
{
void acquire()
{
    if (held)
        return;
    audioMutex.lock();
    held = true;
}

void release()
{
    if (!held)
        return;
    held = false;
    audioMutex.unlock();
}

bool heldByThisThread()
{
    return held;
}
} // namespace AudioLock

// Hooks declared by core (interruptsControl.h, config.h). Masking all interrupts
// on hardware also blocks the audio interrupt, so both map to the same lock.
void disableAudioInterrupt()
{
    AudioLock::acquire();
}

void enableAudioInterrupt()
{
    AudioLock::release();
}

void disableGlobalInterrupts()
{
    AudioLock::acquire();
}

void enableGlobalInterrupts()
{
    AudioLock::release();
}
