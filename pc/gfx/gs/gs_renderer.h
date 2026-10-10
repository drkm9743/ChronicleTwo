// What the GS emulation hands to a renderer: batches of vertices that share
// one drawing state, notices about GS memory the CPU is about to read or has
// just written, and the display registers at each vertical blank. The Vulkan
// backend implements it; tests use NullGsRenderer or their own.

#pragma once

#include "gs_memory.h"

#include <cstddef>
#include <cstdint>

namespace gfx {

/**
 * One vertex in render-target pixels. x and y already have XYOFFSET removed
 * and are not snapped; z is the integer GS depth, clamped to the Z format.
 * With FST the texture coordinates are texels in s and t and q is 1;
 * otherwise s, t and q are the ST and Q registers.
 */
struct GsVertex {
    float    x, y;
    float    s, t, q;
    uint32_t z;
    uint32_t rgba; // R in the low byte, as RGBAQ holds it (0x80 is 1.0)
    uint32_t fog;  // F in the low byte (0xFF is no fog)
};

static_assert(sizeof(GsVertex) == 32, "vertex layout is shared with the shaders");

enum GsTopology : uint8_t {
    TOPOLOGY_POINTS = 0,
    TOPOLOGY_LINES = 1,
    TOPOLOGY_TRIANGLES = 2,
};

/**
 * Everything that decides how a batch is drawn, as raw register values of the
 * context PRIM selects. Fields that cannot matter (texture registers with
 * texturing off, ALPHA with blending off) are zeroed so that equal states
 * compare equal byte for byte and batches merge.
 */
struct GsDrawState {
    uint8_t  topology;
    uint8_t  tme;
    uint8_t  fge;
    uint8_t  abe;
    uint8_t  fst;
    uint8_t  aa1;
    uint8_t  fba;
    uint8_t  colclamp;
    uint64_t frame;
    uint64_t zbuf;
    uint64_t test;
    uint64_t alpha;
    uint64_t tex0; // CLD cleared: loading the CLUT is not part of drawing
    uint64_t tex1;
    uint64_t clamp;
    uint64_t texa;
    uint64_t fogcol;
    uint64_t scissor;
    uint64_t dthe;
    uint64_t pabe;
    uint64_t clut_version; // changes whenever the CLUT buffer does; 0 for non-indexed textures
};

/** The display registers sampled at a vertical blank. */
struct GsDisplayState {
    uint64_t pmode;
    uint64_t smode2;
    uint64_t dispfb[2];
    uint64_t display[2];
    uint64_t bgcolor;
};

class IGsRenderer {
public:
    virtual ~IGsRenderer() = default;

    /** Draws `count` vertices (a whole number of primitives of the state's topology). */
    virtual void Draw(const GsDrawState &state, const GsVertex *vertices, size_t count, const GsClut &clut) = 0;

    /** The CPU is about to read this region of GS memory: write back whatever was drawn there. */
    virtual void SyncToMemory(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) = 0;

    /** The CPU is about to write this region: write back anything a partial write would lose. */
    virtual void PrepareWrite(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) = 0;

    /** The CPU has written this region: render targets over it reload from memory. */
    virtual void MemoryWritten(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) = 0;

    /** A vertical blank: show what the display registers point at. */
    virtual void VSync(const GsDisplayState &display) = 0;
};

/** Draws nothing; memory is always current. For tests and for running without a GPU. */
class NullGsRenderer : public IGsRenderer {
public:
    void Draw(const GsDrawState &, const GsVertex *, size_t, const GsClut &) override {}

    void SyncToMemory(uint32_t, uint32_t, uint32_t, const GsRect &) override {}

    void PrepareWrite(uint32_t, uint32_t, uint32_t, const GsRect &) override {}

    void MemoryWritten(uint32_t, uint32_t, uint32_t, const GsRect &) override {}

    void VSync(const GsDisplayState &) override {}
};

} // namespace gfx
