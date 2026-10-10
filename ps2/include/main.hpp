#pragma once

#include "common.h"

/**
 * @file
 * Declares the program entry point, which brings up the hardware and the IOP and runs the game.
 */

/**
 *
 * Raises the main thread's priority, initialises the graphics, CD and IOP
 * modules, runs the main loop, then shuts the hardware down and returns 0.
 *
 *
 * @mangled main
 * @address 0x15D7B0
 * @size 0x80
 */
#ifdef PLATFORM_PC
/* The PC build has a main of its own (pc/src/main), which starts the game here. */
#define main ps2Main
#endif
int main();

/**
 *
 * VU1 microprogram that draws water surfaces.
 *
 */
extern u_long128 Vu_prog_wtr[];
