// The GS as the rest of the machine sees it: a register file, a vertex queue
// that turns XYZ writes into primitives, the CLUT buffer, image transfers in
// both directions and the privileged display registers. Primitives are
// batched by drawing state and handed to an IGsRenderer.

#pragma once

#include "gs_memory.h"
#include "gs_renderer.h"
#include "gs_types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gfx {

class GsState {
public:
    explicit GsState(GsMemory &memory, IGsRenderer *renderer = nullptr);

    void SetRenderer(IGsRenderer *renderer);

    GsMemory &Memory() { return memory_; }

    const GsClut &Clut() const { return clut_; }

    /** Clears registers and queues, as a GS reset does. Memory is kept. */
    void Reset();

    /** Writes a general register (A+D, REGLIST or HWREG data). */
    void WriteReg(uint32_t reg, uint64_t value);

    uint64_t Reg(uint32_t reg) const { return reg < GS_REG_COUNT ? regs_[reg] : 0; }

    /** Writes one PACKED quadword for register descriptor `desc` (0x0-0xF, 0xE is A+D). */
    void WritePacked(uint32_t desc, const uint32_t qw[4]);

    /** A new GIF tag resets the Q that PACKED RGBAQ uses to 1.0. */
    void ResetQ() { q_ = 1.0f; }

    /** IMAGE-mode data for an active host-to-local transfer. Bytes past the end of the transfer are dropped. */
    void WriteImageData(const uint8_t *data, size_t bytes);

    /** Reads pixels of an active local-to-host transfer; returns the bytes produced. */
    size_t ReadImageData(uint8_t *out, size_t bytes);

    void     WritePrivileged(uint32_t offset, uint64_t value);
    uint64_t ReadPrivileged(uint32_t offset) const;

    /** Sends the pending batch to the renderer. */
    void Flush();

    /** Ends a field: flushes and gives the renderer the display registers. */
    void VSync();

    GsDisplayState Display() const;

private:
    struct Vertex {
        uint32_t x, y; // 12.4 window coordinates
        uint32_t z;
        uint32_t f;
        uint32_t rgba;
        float    s, t, q;
        uint32_t u, v; // 10.4 texels
    };

    struct Transfer {
        int      dir = 3; // TRXDIR: 0 host to local, 1 local to host, 3 none
        uint32_t bp = 0, bw = 0, psm = 0;
        int      x0 = 0, y0 = 0, w = 0, h = 0;
        int      x = 0, y = 0;
        uint64_t carry = 0; // bits of a pixel split between two writes
        int      carry_bits = 0;
    };

    void     Kick(uint32_t reg);
    void     Emit(const Vertex *v, int count, int type);
    void     BuildState(GsDrawState &state, GsTopology topology) const;
    void     AppendVertex(const Vertex &v, const Vertex &color_source, uint32_t zmax, uint32_t ofx, uint32_t ofy);
    void     WriteTex0(int context, uint64_t value);
    void     LoadClutFor(uint64_t tex0);
    void     StartTransfer(uint64_t trxdir);
    void     FinishTransferRows(int y_from, int y_to);
    void     PutPixel(uint32_t value);
    uint32_t TakePixel();
    void     CopyLocal();

    uint32_t PrimAttributes() const;

    int Context() const { return (PrimAttributes() >> 9) & 1; }

    GsMemory      &memory_;
    IGsRenderer   *renderer_;
    NullGsRenderer null_renderer_;

    uint64_t regs_[GS_REG_COUNT] = {};
    uint64_t priv_[0x110] = {}; // privileged registers by offset / 16
    float    q_ = 1.0f;

    Vertex queue_[3] = {};
    int    queued_ = 0;

    GsClut   clut_ = {};
    uint64_t clut_version_ = 1;
    uint32_t cbp0_ = 0, cbp1_ = 0;

    Transfer transfer_;

    GsDrawState           pending_state_ = {};
    std::vector<GsVertex> pending_;
};

} // namespace gfx
