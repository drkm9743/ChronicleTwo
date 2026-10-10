#include "../gs/gs_state.h"
#include "gif.h"

#include <cstring>

namespace gfx {

namespace {

enum GifFlag {
    GIF_PACKED = 0,
    GIF_REGLIST = 1,
    GIF_IMAGE = 2,
    GIF_DISABLE = 3, // behaves as IMAGE
};

} // namespace

Gif::Gif(GsState &gs) : gs_(gs) {}

void Gif::Reset() {
    for (PathState &p : paths_) {
        p = PathState();
    }
}

void Gif::StartTag(PathState &p, const uint32_t qw[4]) {
    uint64_t lo = qw[0] | (static_cast<uint64_t>(qw[1]) << 32);
    uint32_t nloop = lo & 0x7FFF;
    p.eop = (lo >> 15) & 1;
    bool     pre = (lo >> 46) & 1;
    uint32_t prim = (lo >> 47) & 0x7FF;
    p.flg = (lo >> 58) & 3;
    p.nreg = static_cast<uint32_t>(lo >> 60);
    if (p.nreg == 0) {
        p.nreg = 16;
    }
    p.regs = qw[2] | (static_cast<uint64_t>(qw[3]) << 32);
    p.reg_index = 0;

    gs_.ResetQ();
    switch (p.flg) {
        case GIF_PACKED:
            p.remaining = nloop * p.nreg;
            if (pre && nloop != 0) {
                gs_.WriteReg(0x00, prim);
            }
            break;
        case GIF_REGLIST:
            p.remaining = nloop * p.nreg;
            break;
        default:
            p.remaining = nloop;
            break;
    }
}

bool Gif::Quadword(PathState &p, const uint32_t qw[4]) {
    if (p.remaining == 0) {
        StartTag(p, qw);
        return p.remaining == 0 && p.eop;
    }

    switch (p.flg) {
        case GIF_PACKED: {
            uint32_t desc = static_cast<uint32_t>(p.regs >> (p.reg_index * 4)) & 0xF;
            gs_.WritePacked(desc, qw);
            p.reg_index = (p.reg_index + 1) % p.nreg;
            p.remaining--;
            break;
        }
        case GIF_REGLIST:
            // Two registers per quadword; with an odd count the last upper half is padding.
            for (int half = 0; half < 2 && p.remaining > 0; half++) {
                uint32_t desc = static_cast<uint32_t>(p.regs >> (p.reg_index * 4)) & 0xF;
                uint64_t value = qw[half * 2] | (static_cast<uint64_t>(qw[half * 2 + 1]) << 32);
                // REGLIST takes the descriptor as a register address; 0xE (A+D) and 0xF are no-ops.
                if (desc != 0xE && desc != 0xF) {
                    gs_.WriteReg(desc, value);
                }
                p.reg_index = (p.reg_index + 1) % p.nreg;
                p.remaining--;
            }
            break;
        default:
            gs_.WriteImageData(reinterpret_cast<const uint8_t *>(qw), 16);
            p.remaining--;
            break;
    }
    return p.remaining == 0 && p.eop;
}

void Gif::Write(GifPath path, const void *data, size_t qwc) {
    PathState     &p = paths_[path];
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < qwc; i++) {
        uint32_t qw[4];
        std::memcpy(qw, bytes + i * 16, 16);
        Quadword(p, qw);
    }
}

void Gif::Kick(const uint32_t (*vu_mem)[4], uint32_t addr) {
    PathState &p = paths_[GIF_PATH1];
    p = PathState();
    // A runaway packet stops after a full pass over VU memory.
    for (int i = 0; i < 1024 * 4; i++) {
        if (Quadword(p, vu_mem[addr & 0x3FF])) {
            break;
        }
        addr++;
    }
    p = PathState();
}

} // namespace gfx
