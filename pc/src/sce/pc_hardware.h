// What the PC build offers in place of the EE's memory-mapped registers that
// the game reaches without going through the SDK: the GS privileged registers
// (mglib writes DISPFB and DISPLAY itself), DMA channels started by setting
// CHCR.STR (SendDMA on channel 8), and timer 0. Game code calls these behind
// PLATFORM_PC where it wrote the hardware.

#pragma once

#include <libdma.h>

extern "C" {

/** Writes a GS privileged register by its EE address (0x12000000-0x12001FFF). */
void   pcGsWritePrivileged(u_int address, u_long value);
u_long pcGsReadPrivileged(u_int address);

/** Runs the transfer a channel's registers describe, as setting CHCR.STR does. */
void pcDmaStart(sceDmaChan *chan);

/** Timer 0 counting H-blanks (mode 0x83), derived from the host clock. */
u_int pcTimer0Count();
}
