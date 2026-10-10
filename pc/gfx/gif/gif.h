// The GIF: unpacks GIF packets (a tag, then PACKED, REGLIST or IMAGE data)
// into GS register writes. Each of the three paths keeps its own position in
// the current tag, because PATH2 (VIF DIRECT) and PATH3 (GIF DMA) may split a
// packet across several transfers.

#pragma once

#include <cstddef>
#include <cstdint>

namespace gfx {

class GsState;

enum GifPath {
    GIF_PATH1 = 0, // VU1 XGKICK
    GIF_PATH2 = 1, // VIF1 DIRECT / DIRECTHL
    GIF_PATH3 = 2, // GIF channel DMA
};

class Gif {
public:
    explicit Gif(GsState &gs);

    /** Feeds `qwc` quadwords to a path. */
    void Write(GifPath path, const void *data, size_t qwc);

    /**
     * Runs a PATH1 transfer: GIF packets read from VU1 data memory starting at
     * quadword `addr`, up to and including the first tag with EOP.
     */
    void Kick(const uint32_t (*vu_mem)[4], uint32_t addr);

    void Reset();

private:
    struct PathState {
        uint32_t remaining = 0; // quadwords (PACKED, IMAGE) or registers (REGLIST) left in the tag
        uint32_t nreg = 0;
        uint32_t reg_index = 0;
        uint32_t flg = 0;
        bool     eop = false;
        uint64_t regs = 0;
    };

    /** Consumes one quadword; returns true when it ended a packet with EOP. */
    bool Quadword(PathState &p, const uint32_t qw[4]);
    void StartTag(PathState &p, const uint32_t qw[4]);

    GsState  &gs_;
    PathState paths_[3];
};

} // namespace gfx
