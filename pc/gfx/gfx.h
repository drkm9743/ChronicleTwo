// The graphics side of the PS2, wired together: GS memory and registers, the
// GIF, VU1, VIF1 and the DMA channels that feed them. The SDK shims
// (libgraph, libdma, libpkt) talk to one instance of this; the renderer is
// plugged in separately so that the same machine runs with Vulkan, with no
// GPU at all, or under a test.

#pragma once

#include "dma/dma.h"
#include "gif/gif.h"
#include "gs/gs_memory.h"
#include "gs/gs_renderer.h"
#include "gs/gs_state.h"
#include "vif/vif1.h"
#include "vif/vu1_host.h"

#include <cstdint>

namespace gfx {

/** The EE scratchpad: 16 KiB at 0x70000000. */
constexpr uint32_t kScratchpadBytes = 16 * 1024;

class Machine {
public:
    Machine();

    GsMemory memory;
    GsState  gs;
    Gif      gif;
    Vu1Host  vu1;
    Vif1     vif1;
    alignas(64) uint8_t scratchpad[kScratchpadBytes];
    DirectMemoryMap memory_map;
    Dma             dma;

    void SetRenderer(IGsRenderer *renderer) { gs.SetRenderer(renderer); }

    /** Resets the GS, the GIF paths and VIF1, as sceGsResetGraph and sceGsResetPath do. Memory is kept. */
    void Reset();
};

/** The process-wide machine the SDK shims use. */
Machine &GetMachine();

} // namespace gfx
