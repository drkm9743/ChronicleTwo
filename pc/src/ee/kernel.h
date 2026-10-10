#pragma once

// The EE kernel on the PC: threads, semaphores and interrupts as the game
// sees them through eekernel.h.
//
// Every EE thread is a Windows fiber on the process's main thread, and a
// scheduler of our own picks which one runs exactly as the EE kernel does:
// the highest priority ready thread runs until it blocks, rotates its ready
// queue or is overtaken by a higher priority one it woke. Nothing runs two EE
// threads at once.
//
// Interrupts come from a host thread of their own. At each vertical blank it
// takes the kernel lock, suspends the main thread wherever it is (as the EE
// is interrupted between two instructions), runs the handlers the game
// registered and resumes it. The game's DIntr/EIntr pairs hold the same lock.

#include <cstdint>

namespace pc {
namespace ee {

/** Turns the calling host thread into EE thread 1 and starts the interrupt clock. */
void KernelStart(bool pal);

/** Stops the interrupt clock. */
void KernelShutdown();

/** Number of vertical blanks so far. */
uint32_t VBlankCount();

/**
 * Blocks the running EE thread, without letting any other EE thread run,
 * until the vertical blank after `count` has been handled (sceGsSyncV).
 */
void WaitVBlankAfter(uint32_t count);

/** Registers the sceGsSyncVCallback handler, run at the start of every vertical blank. */
void SetVSyncCallback(int (*callback)(int));

/** Sends DMA end-of-transfer interrupts for `channel` to its handlers (from the game's thread). */
void RaiseDmac(int channel);

/** Called on the interrupt thread with the main thread stopped, at each vertical blank. */
void SetVBlankHook(void (*hook)());

} // namespace ee
} // namespace pc
