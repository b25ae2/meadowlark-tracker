#pragma once

// Emulates the firmware's "audio interrupt" exclusion on a host with real threads.
//
// On hardware, the audio DMA interrupt pre-empts the main loop, and the main loop
// masks that interrupt (NVIC or CPSID) around shared state. NVIC enable/disable does
// not nest: disabling twice and enabling once re-enables. AudioLock keeps those
// semantics: acquire() is a no-op if this thread already holds the lock, and
// release() is a no-op if it does not.
//
// The audio render thread holds the lock for each block it renders, standing in
// for the interrupt handler running with the main loop suspended.
namespace AudioLock
{
void acquire();
void release();
bool heldByThisThread();
} // namespace AudioLock
