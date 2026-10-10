// Dark Cloud 2's VU1 microprograms, as the C++ translations (written and
// checked bit for bit against the microcode elsewhere; see vu_progs.h),
// hooked into Vu1Host. Each is registered by the code the game uploads, so
// MSCAL runs the translation of whatever program the game loaded:
//
//   Vu_progmain   resident at 0x000: MSCAL 0 sets up the frame (lights,
//                 matrices); other entries run the overlay at 0x064
//   Vu_prog0      models          Vu_prog_sdw   shadow volumes
//   Vu_prog_3dsp  3D sprites      Vu_prog_wtr   water
//
// The translations need round-toward-zero with denormals flushed, as the VU
// computes; the adapter sets that around each call and restores the host's
// mode afterwards.

#pragma once

#include "../vif/vu1_host.h"

namespace gfx {

/** The game's MPG upload packets (DMA tag + VIF codes), as sent to channel 1. */
struct Dc2ProgramPackets {
    const void *progmain = nullptr;
    const void *model = nullptr;    // Vu_prog0
    const void *shadow = nullptr;   // Vu_prog_sdw
    const void *sprite3d = nullptr; // Vu_prog_3dsp
    const void *water = nullptr;    // Vu_prog_wtr
};

/** Registers every program whose packet is given. Returns how many were registered. */
int RegisterDc2Programs(Vu1Host &host, const Dc2ProgramPackets &packets);

} // namespace gfx
