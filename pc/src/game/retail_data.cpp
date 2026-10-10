// Data the PS2 build takes from the retail executable as assembled bytes
// rather than from the sources: the VU1 microprograms, which the game uploads
// as DMA packets, and the DMA chain mgInit sends first. The PC build reads
// them from the executable on the player's disc at start-up, so none of
// Sony's or Level-5's bytes live in this repository.
//
// Compiled with the game, so the definitions match the game's declarations.

#include "common.h"
#include "main.hpp"
#include "mglib.hpp"

extern "C" int pcReadRetailExecutable(u_int address, void *buffer, u_int size);

/** Address in the retail executable, and size, of each block (ps2/config/pal/main.symbols.txt). */
#define RETAIL_BLOCK(NAME, ADDRESS, SIZE)                                                               \
    u_long128 NAME[(SIZE) / 16] __attribute__((aligned(16)));                                           \
    static const u_int NAME##_address = ADDRESS;                                                        \
    static const u_int NAME##_size = SIZE

RETAIL_BLOCK(Vu_prog_3dsp, 0x325C80, 0x6D0);
RETAIL_BLOCK(Vu_prog_sdw, 0x326350, 0x970);
RETAIL_BLOCK(Vu_progmain, 0x326CC0, 0x2D0);
RETAIL_BLOCK(Vu_prog0, 0x326F90, 0x3040);
RETAIL_BLOCK(Vu_prog_wtr, 0x329FD0, 0x350);
// Two CNT tags (VIF1 setup and the drawing environment, sent DIRECT) and an END.
RETAIL_BLOCK(My_dma_start0, 0x363480, 0xE0);

#define LOAD(NAME) ok &= pcReadRetailExecutable(NAME##_address, NAME, NAME##_size)

int pcLoadRetailData() {
    int ok = 1;
    LOAD(Vu_prog_3dsp);
    LOAD(Vu_prog_sdw);
    LOAD(Vu_progmain);
    LOAD(Vu_prog0);
    LOAD(Vu_prog_wtr);
    LOAD(My_dma_start0);
    return ok;
}

/** The microprogram packets, for the PC to register its C++ versions of them. */
void pcRetailVuPrograms(const void *packets[5]) {
    packets[0] = Vu_progmain;
    packets[1] = Vu_prog0;
    packets[2] = Vu_prog_sdw;
    packets[3] = Vu_prog_3dsp;
    packets[4] = Vu_prog_wtr;
}
