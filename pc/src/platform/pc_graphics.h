// The PC side of the PS2's video output: the game window, the Vulkan renderer
// behind the emulated GS, and the field clock that sceGsSyncV waits on. The
// libgraph shim starts it from sceGsResetGraph; nothing else needs to.

#pragma once

namespace gfx {
class Machine;
}

namespace pc {

/**
 * Opens the window and the renderer the first time it is called; later calls
 * only change the field rate. Without a usable Vulkan device, or with the
 * environment variable CHRONICLETWO_HEADLESS set, the GS runs with no
 * renderer: drawing is dropped and memory transfers still work.
 */
void GraphicsStart(bool pal);

void GraphicsShutdown();

/**
 * Ends a field: presents what the display registers point at, handles window
 * events and waits until the next field is due (50 Hz PAL, 59.94 Hz NTSC).
 */
void GraphicsVSync();

/**
 * Ends a field without waiting: presents and handles window events. For a
 * caller whose fields are paced by the EE kernel's vertical blank interrupt.
 */
void GraphicsPresent();

/** The graphics side of the PS2 the shims drive. */
gfx::Machine &GraphicsMachine();

} // namespace pc
