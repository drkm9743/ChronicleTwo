#include "gs_state.h"

#include <algorithm>
#include <cstring>

namespace gfx {

namespace {

float AsFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint32_t AsBits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

bool IsIndexed(uint32_t psm) {
    return psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
}

uint32_t ClutCount(uint32_t psm) {
    if (psm == PSMT8 || psm == PSMT8H) {
        return 256;
    }
    if (psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH) {
        return 16;
    }
    return 0;
}

/** TEX2 replaces PSM and the CLUT fields of TEX0 and leaves the rest. */
const uint64_t kTex2Mask = (uint64_t(0x3F) << 20) | (~uint64_t(0) << 37);

const uint32_t kCsrField = 1u << 13;
const uint32_t kCsrReset = 1u << 9;

} // namespace

GsState::GsState(GsMemory &memory, IGsRenderer *renderer)
    : memory_(memory), renderer_(renderer ? renderer : &null_renderer_) {
    Reset();
}

void GsState::SetRenderer(IGsRenderer *renderer) {
    Flush();
    renderer_ = renderer ? renderer : &null_renderer_;
}

void GsState::Reset() {
    pending_.clear();
    std::memset(regs_, 0, sizeof(regs_));
    // PRMODECONT starts with AC = 1: attributes come from PRIM.
    regs_[GS_PRMODECONT] = 1;
    q_ = 1.0f;
    queued_ = 0;
    transfer_ = Transfer();
    pending_state_ = GsDrawState();
}

uint32_t GsState::PrimAttributes() const {
    uint64_t source = (regs_[GS_PRMODECONT] & 1) ? regs_[GS_PRIM] : regs_[GS_PRMODE];
    return static_cast<uint32_t>(source & 0x7F8);
}

void GsState::WriteReg(uint32_t reg, uint64_t value) {
    if (reg >= GS_REG_COUNT) {
        return;
    }

    switch (reg) {
        case GS_PRIM:
            regs_[reg] = value;
            queued_ = 0;
            return;
        case GS_XYZF2:
        case GS_XYZF3:
        case GS_XYZ2:
        case GS_XYZ3:
            regs_[reg] = value;
            Kick(reg);
            return;
        case GS_TEX0_1:
        case GS_TEX0_2:
            WriteTex0(reg - GS_TEX0_1, value);
            return;
        case GS_TEX2_1:
        case GS_TEX2_2: {
            int      context = reg - GS_TEX2_1;
            uint64_t tex0 = regs_[GS_TEX0_1 + context];
            WriteTex0(context, (tex0 & ~kTex2Mask) | (value & kTex2Mask));
            return;
        }
        case GS_TRXDIR:
            regs_[reg] = value;
            StartTransfer(value);
            return;
        case GS_HWREG: {
            uint8_t bytes[8];
            std::memcpy(bytes, &value, 8);
            WriteImageData(bytes, 8);
            return;
        }
        case GS_SIGNAL:
        case GS_FINISH:
        case GS_LABEL:
            regs_[reg] = value;
            return;
        default:
            regs_[reg] = value;
            return;
    }
}

void GsState::WritePacked(uint32_t desc, const uint32_t qw[4]) {
    switch (desc & 0xF) {
        case 0x0: // PRIM
            WriteReg(GS_PRIM, qw[0] & 0x7FF);
            break;
        case 0x1: { // RGBAQ: Q is the one the last PACKED ST left
            uint64_t rgba = (qw[0] & 0xFF) | ((qw[1] & 0xFF) << 8) | ((qw[2] & 0xFF) << 16) | ((qw[3] & 0xFF) << 24);
            regs_[GS_RGBAQ] = rgba | (static_cast<uint64_t>(AsBits(q_)) << 32);
            break;
        }
        case 0x2: // ST, and Q for the next RGBAQ
            regs_[GS_ST] = qw[0] | (static_cast<uint64_t>(qw[1]) << 32);
            q_ = AsFloat(qw[2]);
            break;
        case 0x3: // UV
            regs_[GS_UV] = (qw[0] & 0x3FFF) | (static_cast<uint64_t>(qw[1] & 0x3FFF) << 16);
            break;
        case 0x4: // XYZF2, or XYZF3 when ADC is set
        case 0xC: {
            uint64_t v = (qw[0] & 0xFFFF) | (static_cast<uint64_t>(qw[1] & 0xFFFF) << 16) |
                         (static_cast<uint64_t>((qw[2] >> 4) & 0xFFFFFF) << 32) |
                         (static_cast<uint64_t>((qw[3] >> 4) & 0xFF) << 56);
            bool adc = (desc & 0xF) == 0xC || ((qw[3] >> 15) & 1);
            WriteReg(adc ? GS_XYZF3 : GS_XYZF2, v);
            break;
        }
        case 0x5: // XYZ2, or XYZ3 when ADC is set
        case 0xD: {
            uint64_t v = (qw[0] & 0xFFFF) | (static_cast<uint64_t>(qw[1] & 0xFFFF) << 16) |
                         (static_cast<uint64_t>(qw[2]) << 32);
            bool adc = (desc & 0xF) == 0xD || ((qw[3] >> 15) & 1);
            WriteReg(adc ? GS_XYZ3 : GS_XYZ2, v);
            break;
        }
        case 0x6:
        case 0x7:
        case 0x8:
        case 0x9:
            WriteReg(desc & 0xF, qw[0] | (static_cast<uint64_t>(qw[1]) << 32));
            break;
        case 0xA: // FOG
            regs_[GS_FOG] = static_cast<uint64_t>((qw[3] >> 4) & 0xFF) << 56;
            break;
        case 0xE: // A+D
            WriteReg(qw[2] & 0xFF, qw[0] | (static_cast<uint64_t>(qw[1]) << 32));
            break;
        default: // 0xB reserved, 0xF NOP
            break;
    }
}

void GsState::Kick(uint32_t reg) {
    bool     draw = reg == GS_XYZF2 || reg == GS_XYZ2;
    uint64_t value = regs_[reg];
    Vertex   v;
    v.x = Bits(value, 0, 16);
    v.y = Bits(value, 16, 16);
    if (reg == GS_XYZF2 || reg == GS_XYZF3) {
        v.z = Bits(value, 32, 24);
        v.f = Bits(value, 56, 8);
    } else {
        v.z = Bits(value, 32, 32);
        v.f = Bits(regs_[GS_FOG], 56, 8);
    }
    v.rgba = static_cast<uint32_t>(regs_[GS_RGBAQ]);
    v.q = AsFloat(static_cast<uint32_t>(regs_[GS_RGBAQ] >> 32));
    v.s = AsFloat(static_cast<uint32_t>(regs_[GS_ST]));
    v.t = AsFloat(static_cast<uint32_t>(regs_[GS_ST] >> 32));
    v.u = Bits(regs_[GS_UV], 0, 14);
    v.v = Bits(regs_[GS_UV], 16, 14);

    int type = static_cast<int>(regs_[GS_PRIM] & 7);
    queue_[queued_++] = v;

    switch (type) {
        case PRIM_POINT:
            if (draw) {
                Emit(queue_, 1, type);
            }
            queued_ = 0;
            break;
        case PRIM_LINE:
        case PRIM_SPRITE:
            if (queued_ == 2) {
                if (draw) {
                    Emit(queue_, 2, type);
                }
                queued_ = 0;
            }
            break;
        case PRIM_LINE_STRIP:
            if (queued_ == 2) {
                if (draw) {
                    Emit(queue_, 2, type);
                }
                queue_[0] = queue_[1];
                queued_ = 1;
            }
            break;
        case PRIM_TRIANGLE:
            if (queued_ == 3) {
                if (draw) {
                    Emit(queue_, 3, type);
                }
                queued_ = 0;
            }
            break;
        case PRIM_TRIANGLE_STRIP:
            if (queued_ == 3) {
                if (draw) {
                    Emit(queue_, 3, type);
                }
                queue_[0] = queue_[1];
                queue_[1] = queue_[2];
                queued_ = 2;
            }
            break;
        case PRIM_TRIANGLE_FAN:
            if (queued_ == 3) {
                if (draw) {
                    Emit(queue_, 3, type);
                }
                queue_[1] = queue_[2];
                queued_ = 2;
            }
            break;
        default:
            queued_ = 0;
            break;
    }
}

void GsState::BuildState(GsDrawState &state, GsTopology topology) const {
    std::memset(&state, 0, sizeof(state));
    uint32_t attr = PrimAttributes();
    int      ctx = (attr >> 9) & 1;
    state.topology = topology;
    state.tme = (attr >> 4) & 1;
    state.fge = (attr >> 5) & 1;
    state.abe = (attr >> 6) & 1;
    state.aa1 = (attr >> 7) & 1;
    state.fba = regs_[GS_FBA_1 + ctx] & 1;
    state.colclamp = regs_[GS_COLCLAMP] & 1;
    state.frame = regs_[GS_FRAME_1 + ctx];
    state.zbuf = regs_[GS_ZBUF_1 + ctx];
    state.test = regs_[GS_TEST_1 + ctx];
    state.scissor = regs_[GS_SCISSOR_1 + ctx];
    state.dthe = regs_[GS_DTHE] & 1;
    if (state.abe) {
        state.alpha = regs_[GS_ALPHA_1 + ctx];
        state.pabe = regs_[GS_PABE] & 1;
    }
    if (state.fge) {
        state.fogcol = regs_[GS_FOGCOL] & 0xFFFFFF;
    }
    if (state.tme) {
        state.fst = (attr >> 8) & 1;
        state.tex0 = regs_[GS_TEX0_1 + ctx] & ~(uint64_t(7) << 61);
        state.tex1 = regs_[GS_TEX1_1 + ctx];
        state.clamp = regs_[GS_CLAMP_1 + ctx];
        state.texa = regs_[GS_TEXA];
        if (IsIndexed(Bits(state.tex0, 20, 6))) {
            state.clut_version = clut_version_;
        }
    }
}

void GsState::AppendVertex(const Vertex &v, const Vertex &color_source, uint32_t zmax, uint32_t ofx,
                           uint32_t ofy) {
    GsVertex out;
    out.x = (static_cast<int>(v.x) - static_cast<int>(ofx)) / 16.0f;
    out.y = (static_cast<int>(v.y) - static_cast<int>(ofy)) / 16.0f;
    out.z = std::min(v.z, zmax);
    out.rgba = color_source.rgba;
    out.fog = v.f;
    if (pending_state_.fst) {
        out.s = v.u / 16.0f;
        out.t = v.v / 16.0f;
        out.q = 1.0f;
    } else {
        out.s = v.s;
        out.t = v.t;
        out.q = v.q;
    }
    pending_.push_back(out);
}

void GsState::Emit(const Vertex *v, int count, int type) {
    GsTopology  topology = type == PRIM_POINT                               ? TOPOLOGY_POINTS
                           : (type == PRIM_LINE || type == PRIM_LINE_STRIP) ? TOPOLOGY_LINES
                                                                            : TOPOLOGY_TRIANGLES;
    GsDrawState state;
    BuildState(state, topology);
    if (std::memcmp(&state, &pending_state_, sizeof(state)) != 0) {
        Flush();
        pending_state_ = state;
    }

    uint32_t      attr = PrimAttributes();
    int           ctx = (attr >> 9) & 1;
    bool          gouraud = (attr >> 3) & 1;
    uint64_t      offset = regs_[GS_XYOFFSET_1 + ctx];
    uint32_t      ofx = Bits(offset, 0, 16);
    uint32_t      ofy = Bits(offset, 32, 16);
    uint32_t      zmax = ZFormatMax(Bits(state.zbuf, 24, 4));
    const Vertex &last = v[count - 1];

    if (type == PRIM_SPRITE) {
        // Two opposite corners; colour, Z and fog come from the second. Texture
        // coordinates are resolved to S/Q and T/Q so the corners can mix them.
        Vertex a = v[0], b = v[1];
        a.z = b.z;
        a.f = b.f;
        if (!pending_state_.fst) {
            float qa = a.q != 0.0f ? a.q : 1.0f;
            float qb = b.q != 0.0f ? b.q : 1.0f;
            a.s /= qa;
            a.t /= qa;
            b.s /= qb;
            b.t /= qb;
            a.q = b.q = 1.0f;
        }
        Vertex c = a, d = a;
        c.x = b.x;
        c.s = b.s;
        c.u = b.u; // (x1, y0)
        d.y = b.y;
        d.t = b.t;
        d.v = b.v; // (x0, y1)
        const Vertex *corners[6] = {&a, &c, &d, &c, &b, &d};
        for (const Vertex *corner : corners) {
            AppendVertex(*corner, b, zmax, ofx, ofy);
        }
        return;
    }

    for (int i = 0; i < count; i++) {
        AppendVertex(v[i], gouraud ? v[i] : last, zmax, ofx, ofy);
    }
}

void GsState::Flush() {
    if (!pending_.empty()) {
        renderer_->Draw(pending_state_, pending_.data(), pending_.size(), clut_);
        pending_.clear();
    }
}

void GsState::WriteTex0(int context, uint64_t value) {
    regs_[GS_TEX0_1 + context] = value;
    LoadClutFor(value);
}

void GsState::LoadClutFor(uint64_t tex0) {
    uint32_t psm = Bits(tex0, 20, 6);
    uint32_t cbp = Bits(tex0, 37, 14);
    uint32_t cpsm = Bits(tex0, 51, 4);
    uint32_t csm = Bits(tex0, 55, 1);
    uint32_t csa = Bits(tex0, 56, 5);
    uint32_t cld = Bits(tex0, 61, 3);
    uint32_t count = ClutCount(psm);
    if (count == 0) {
        return;
    }

    bool load = false;
    switch (cld) {
        case 1:
            load = true;
            break;
        case 2:
            load = true;
            cbp0_ = cbp;
            break;
        case 3:
            load = true;
            cbp1_ = cbp;
            break;
        case 4:
            load = cbp != cbp0_;
            cbp0_ = cbp;
            break;
        case 5:
            load = cbp != cbp1_;
            cbp1_ = cbp;
            break;
        default:
            break;
    }
    if (!load) {
        return;
    }

    // Draws already queued use the CLUT as it is now.
    Flush();
    uint32_t read_psm = (cpsm == PSMCT32 || cpsm == PSMCT24) ? uint32_t(PSMCT32) : cpsm;
    GsRect   rect = count == 16 ? GsRect{0, 0, 8, 2} : GsRect{0, 0, 16, 16};
    uint32_t bw = 1;
    if (csm) {
        uint64_t texclut = regs_[GS_TEXCLUT];
        bw = Bits(texclut, 0, 6);
        int x = static_cast<int>(Bits(texclut, 6, 6) * 16);
        int y = static_cast<int>(Bits(texclut, 12, 10));
        rect = GsRect{x, y, x + static_cast<int>(count), y + 1};
        read_psm = PSMCT16;
    }
    renderer_->SyncToMemory(cbp, bw, read_psm, rect);

    GsClut loaded = clut_;
    memory_.LoadClut(loaded, cbp, cpsm, csm, csa, count, regs_[GS_TEXCLUT]);
    if (std::memcmp(&loaded, &clut_, sizeof(clut_)) != 0) {
        clut_ = loaded;
        clut_version_++;
    }
}

void GsState::StartTransfer(uint64_t trxdir) {
    Flush();
    uint64_t bitblt = regs_[GS_BITBLTBUF];
    uint64_t pos = regs_[GS_TRXPOS];
    uint64_t reg = regs_[GS_TRXREG];
    Transfer t;
    t.dir = static_cast<int>(trxdir & 3);
    t.w = static_cast<int>(Bits(reg, 0, 12));
    t.h = static_cast<int>(Bits(reg, 32, 12));

    if (t.dir == 0) {
        t.bp = Bits(bitblt, 32, 14);
        t.bw = Bits(bitblt, 48, 6);
        t.psm = Bits(bitblt, 56, 6);
        t.x0 = static_cast<int>(Bits(pos, 32, 11));
        t.y0 = static_cast<int>(Bits(pos, 48, 11));
        transfer_ = t;
        renderer_->PrepareWrite(t.bp, t.bw, t.psm, GsRect{t.x0, t.y0, t.x0 + t.w, t.y0 + t.h});
    } else if (t.dir == 1) {
        t.bp = Bits(bitblt, 0, 14);
        t.bw = Bits(bitblt, 16, 6);
        t.psm = Bits(bitblt, 24, 6);
        t.x0 = static_cast<int>(Bits(pos, 0, 11));
        t.y0 = static_cast<int>(Bits(pos, 16, 11));
        transfer_ = t;
        renderer_->SyncToMemory(t.bp, t.bw, t.psm, GsRect{t.x0, t.y0, t.x0 + t.w, t.y0 + t.h});
    } else if (t.dir == 2) {
        transfer_ = Transfer();
        CopyLocal();
    } else {
        transfer_ = Transfer();
    }
}

void GsState::PutPixel(uint32_t value) {
    Transfer &t = transfer_;
    memory_.WritePixel(t.psm, t.bp, t.bw, (t.x0 + t.x) & 2047, (t.y0 + t.y) & 2047, value);
    if (++t.x == t.w) {
        t.x = 0;
        t.y++;
    }
}

void GsState::FinishTransferRows(int y_from, int y_to) {
    Transfer &t = transfer_;
    if (y_to <= y_from) {
        return;
    }
    GsRect rect{t.x0, t.y0 + y_from, t.x0 + t.w, t.y0 + y_to};
    memory_.MarkWritten(t.bp, t.bw, t.psm, rect);
    renderer_->MemoryWritten(t.bp, t.bw, t.psm, rect);
}

void GsState::WriteImageData(const uint8_t *data, size_t bytes) {
    Transfer &t = transfer_;
    if (t.dir != 0 || t.w == 0 || t.h == 0) {
        return;
    }
    int      bits = PsmTransferBits(t.psm);
    uint64_t mask = (uint64_t(1) << bits) - 1;
    int      y_from = t.y;
    for (size_t i = 0; i < bytes && t.y < t.h; i++) {
        t.carry |= static_cast<uint64_t>(data[i]) << t.carry_bits;
        t.carry_bits += 8;
        while (t.carry_bits >= bits && t.y < t.h) {
            PutPixel(static_cast<uint32_t>(t.carry & mask));
            t.carry >>= bits;
            t.carry_bits -= bits;
        }
    }
    FinishTransferRows(y_from, t.x > 0 ? t.y + 1 : t.y);
    if (t.y >= t.h) {
        t.dir = 3;
    }
}

uint32_t GsState::TakePixel() {
    Transfer &t = transfer_;
    uint32_t  v = memory_.ReadPixel(t.psm, t.bp, t.bw, (t.x0 + t.x) & 2047, (t.y0 + t.y) & 2047);
    if (++t.x == t.w) {
        t.x = 0;
        t.y++;
    }
    return v;
}

size_t GsState::ReadImageData(uint8_t *out, size_t bytes) {
    Transfer &t = transfer_;
    if (t.dir != 1) {
        return 0;
    }
    int      bits = PsmTransferBits(t.psm);
    uint64_t mask = (uint64_t(1) << bits) - 1;
    size_t   produced = 0;
    while (produced < bytes) {
        while (t.carry_bits < 8 && t.y < t.h) {
            t.carry |= (static_cast<uint64_t>(TakePixel()) & mask) << t.carry_bits;
            t.carry_bits += bits;
        }
        if (t.carry_bits < 8) {
            if (t.carry_bits > 0) {
                out[produced++] = static_cast<uint8_t>(t.carry);
                t.carry = 0;
                t.carry_bits = 0;
            }
            break;
        }
        out[produced++] = static_cast<uint8_t>(t.carry);
        t.carry >>= 8;
        t.carry_bits -= 8;
    }
    if (t.y >= t.h && t.carry_bits == 0) {
        t.dir = 3;
    }
    return produced;
}

void GsState::CopyLocal() {
    uint64_t bitblt = regs_[GS_BITBLTBUF];
    uint64_t pos = regs_[GS_TRXPOS];
    uint64_t reg = regs_[GS_TRXREG];
    uint32_t sbp = Bits(bitblt, 0, 14), sbw = Bits(bitblt, 16, 6), spsm = Bits(bitblt, 24, 6);
    uint32_t dbp = Bits(bitblt, 32, 14), dbw = Bits(bitblt, 48, 6), dpsm = Bits(bitblt, 56, 6);
    int      sx = static_cast<int>(Bits(pos, 0, 11)), sy = static_cast<int>(Bits(pos, 16, 11));
    int      dx = static_cast<int>(Bits(pos, 32, 11)), dy = static_cast<int>(Bits(pos, 48, 11));
    int      w = static_cast<int>(Bits(reg, 0, 12)), h = static_cast<int>(Bits(reg, 32, 12));
    if (w == 0 || h == 0) {
        return;
    }
    GsRect src{sx, sy, sx + w, sy + h};
    GsRect dst{dx, dy, dx + w, dy + h};
    renderer_->SyncToMemory(sbp, sbw, spsm, src);
    renderer_->PrepareWrite(dbp, dbw, dpsm, dst);

    // Through a copy, so overlapping rectangles behave as if read before written.
    std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            pixels[y * w + x] = memory_.ReadPixel(spsm, sbp, sbw, (sx + x) & 2047, (sy + y) & 2047);
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            memory_.WritePixel(dpsm, dbp, dbw, (dx + x) & 2047, (dy + y) & 2047, pixels[y * w + x]);
        }
    }
    memory_.MarkWritten(dbp, dbw, dpsm, dst);
    renderer_->MemoryWritten(dbp, dbw, dpsm, dst);
}

void GsState::WritePrivileged(uint32_t offset, uint64_t value) {
    uint32_t index = (offset >> 4) % (sizeof(priv_) / sizeof(priv_[0]));
    if (offset == GS_PRIV_CSR) {
        if (value & kCsrReset) {
            Reset();
        }
        // The interrupt flags clear when written with 1; the rest is control.
        priv_[index] = (priv_[index] & kCsrField) | (value & ~uint64_t(0x1F) & ~uint64_t(kCsrReset) & ~uint64_t(kCsrField));
        return;
    }
    priv_[index] = value;
}

uint64_t GsState::ReadPrivileged(uint32_t offset) const {
    return priv_[(offset >> 4) % (sizeof(priv_) / sizeof(priv_[0]))];
}

GsDisplayState GsState::Display() const {
    GsDisplayState d;
    d.pmode = ReadPrivileged(GS_PRIV_PMODE);
    d.smode2 = ReadPrivileged(GS_PRIV_SMODE2);
    d.dispfb[0] = ReadPrivileged(GS_PRIV_DISPFB1);
    d.dispfb[1] = ReadPrivileged(GS_PRIV_DISPFB2);
    d.display[0] = ReadPrivileged(GS_PRIV_DISPLAY1);
    d.display[1] = ReadPrivileged(GS_PRIV_DISPLAY2);
    d.bgcolor = ReadPrivileged(GS_PRIV_BGCOLOR);
    return d;
}

void GsState::VSync() {
    Flush();
    priv_[GS_PRIV_CSR >> 4] ^= kCsrField;
    renderer_->VSync(Display());
}

} // namespace gfx
