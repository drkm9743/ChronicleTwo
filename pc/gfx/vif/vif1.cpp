#include "../gif/gif.h"
#include "vif1.h"
#include "vu1_host.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gfx {

namespace {

enum VifCmd : uint32_t {
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

/** Bytes of one input vector: vn + 1 elements of 32 >> vl bits; V4-5 packs a vector in 16 bits. */
uint32_t VectorBytes(uint32_t vn, uint32_t vl) {
    if (vl == 3) {
        return 2;
    }
    return (vn + 1) * (4 >> vl);
}

} // namespace

Vif1::Vif1(Vu1Host &vu, Gif &gif) : vu_(vu), gif_(gif) {}

void Vif1::Reset() {
    cl_ = wl_ = 1;
    mode_ = mask_ = 0;
    std::memset(row_, 0, sizeof(row_));
    std::memset(col_, 0, sizeof(col_));
    base_ = ofst_ = tops_ = itop_ = itops_ = 0;
    dbf_ = false;
    pending_ = NONE;
    words_left_ = 0;
}

void Vif1::Write(const uint32_t *words, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (pending_ == NONE) {
            Code(words[i]);
        } else {
            Data(words[i]);
        }
    }
}

void Vif1::StartProgram(uint32_t pc, bool resume) {
    // Double buffering: the program sees the buffer the last UNPACKs filled,
    // and the next UNPACKs with FLG go to the other one.
    vu_.State().top = tops_;
    vu_.State().itop = itops_;
    dbf_ = !dbf_;
    tops_ = dbf_ ? base_ + ofst_ : base_;
    if (resume) {
        vu_.Continue();
    } else {
        vu_.Start(pc);
    }
}

void Vif1::Code(uint32_t code) {
    uint32_t cmd = (code >> 24) & 0x7F; // bit 7 is the interrupt request
    uint32_t num = (code >> 16) & 0xFF;
    uint32_t imm = code & 0xFFFF;

    if ((cmd & 0x60) == 0x60) {
        StartUnpack(code);
        return;
    }

    switch (cmd) {
        case VIF_NOP:
        case VIF_MSKPATH3:
        case VIF_MARK:
        case VIF_FLUSHE:
        case VIF_FLUSH:
        case VIF_FLUSHA:
            // Everything runs to completion as it is issued, so there is nothing to wait for.
            break;
        case VIF_STCYCL:
            cl_ = imm & 0xFF;
            wl_ = (imm >> 8) & 0xFF;
            break;
        case VIF_OFFSET:
            ofst_ = imm & 0x3FF;
            dbf_ = false;
            tops_ = base_;
            break;
        case VIF_BASE:
            base_ = imm & 0x3FF;
            break;
        case VIF_ITOP:
            itops_ = imm & 0x3FF;
            break;
        case VIF_STMOD:
            mode_ = imm & 3;
            break;
        case VIF_MSCAL:
        case VIF_MSCALF:
            StartProgram(imm, false);
            break;
        case VIF_MSCNT:
            StartProgram(0, true);
            break;
        case VIF_STMASK:
            pending_ = STMASK;
            words_left_ = 1;
            break;
        case VIF_STROW:
            pending_ = STROW;
            words_left_ = 4;
            index_ = 0;
            break;
        case VIF_STCOL:
            pending_ = STCOL;
            words_left_ = 4;
            index_ = 0;
            break;
        case VIF_MPG:
            pending_ = MPG;
            words_left_ = (num == 0 ? 256 : num) * 2;
            mpg_addr_ = imm;
            index_ = 0;
            break;
        case VIF_DIRECT:
        case VIF_DIRECTHL:
            pending_ = DIRECT;
            words_left_ = (imm == 0 ? 65536u : imm) * 4;
            index_ = 0;
            break;
        default:
            std::fprintf(stderr, "gfx: unknown VIF code 0x%08X ignored\n", code);
            break;
    }
}

void Vif1::Data(uint32_t word) {
    switch (pending_) {
        case STMASK:
            mask_ = word;
            break;
        case STROW:
            row_[index_++] = word;
            break;
        case STCOL:
            col_[index_++] = word;
            break;
        case MPG:
            if ((index_ & 1) == 0) {
                mpg_low_ = word;
            } else {
                uint64_t instr = mpg_low_ | (static_cast<uint64_t>(word) << 32);
                vu_.LoadMicro(mpg_addr_ + index_ / 2, &instr, 1);
            }
            index_++;
            break;
        case DIRECT:
            direct_qw_[index_++] = word;
            if (index_ == 4) {
                gif_.Write(GIF_PATH2, direct_qw_, 1);
                index_ = 0;
            }
            break;
        case UNPACK: {
            std::memcpy(unpack_.buffer + unpack_.buffered, &word, 4);
            unpack_.buffered += 4;
            RunUnpack();
            break;
        }
        default:
            break;
    }

    if (pending_ != UNPACK && --words_left_ == 0) {
        pending_ = NONE;
    }
}

void Vif1::StartUnpack(uint32_t code) {
    uint32_t cmd = (code >> 24) & 0x7F;
    uint32_t num = (code >> 16) & 0xFF;
    Unpack  &u = unpack_;
    u = Unpack();
    u.vl = cmd & 3;
    u.vn = (cmd >> 2) & 3;
    u.masked = (cmd >> 4) & 1;
    u.usn = (code >> 14) & 1;
    u.num = num == 0 ? 256 : num;
    u.addr = code & 0x3FF;
    if ((code >> 15) & 1) {
        u.addr += tops_;
    }
    u.vector_bytes = VectorBytes(u.vn, u.vl);

    // Input vectors: every write cycle reads one, except that filling (WL > CL)
    // reads only the first CL of each WL.
    uint32_t inputs = u.num;
    if (wl_ > cl_) {
        inputs = (u.num / wl_) * cl_ + std::min(u.num % wl_, cl_);
    }
    words_left_ = (inputs * u.vector_bytes + 3) / 4;
    pending_ = UNPACK;
    RunUnpack();
}

void Vif1::RunUnpack() {
    Unpack &u = unpack_;
    while (u.written < u.num) {
        bool needs_data = !(wl_ > cl_ && (u.written % wl_) >= cl_);
        if (needs_data) {
            if (u.buffered < u.vector_bytes) {
                break;
            }
            WriteVector(u.written, reinterpret_cast<const uint32_t *>(u.buffer), true);
            std::memmove(u.buffer, u.buffer + u.vector_bytes, u.buffered - u.vector_bytes);
            u.buffered -= u.vector_bytes;
        } else {
            WriteVector(u.written, nullptr, false);
        }
        u.written++;
    }

    if (u.written == u.num) {
        // The rest of the last word is padding.
        words_left_ = 0;
        pending_ = NONE;
        u.buffered = 0;
    }
}

void Vif1::WriteVector(uint32_t n, const uint32_t *data, bool has_data) {
    Unpack  &u = unpack_;
    uint32_t addr;
    if (wl_ <= cl_) {
        uint32_t wl = wl_ == 0 ? 1 : wl_;
        addr = u.addr + (n / wl) * cl_ + (n % wl);
    } else {
        addr = u.addr + n;
    }

    // Decode the input vector into up to four 32-bit fields.
    uint32_t in[4] = {};
    if (has_data) {
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
        if (u.vl == 3) {
            uint16_t v;
            std::memcpy(&v, bytes, 2);
            in[0] = (v & 0x1F) << 3;
            in[1] = ((v >> 5) & 0x1F) << 3;
            in[2] = ((v >> 10) & 0x1F) << 3;
            in[3] = ((v >> 15) & 1) << 7;
        } else {
            uint32_t count = u.vn + 1;
            for (uint32_t i = 0; i < count; i++) {
                if (u.vl == 0) {
                    std::memcpy(&in[i], bytes + i * 4, 4);
                } else if (u.vl == 1) {
                    uint16_t h;
                    std::memcpy(&h, bytes + i * 2, 2);
                    in[i] = u.usn ? h : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(h)));
                } else {
                    uint8_t b = bytes[i];
                    in[i] = u.usn ? b : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(b)));
                }
            }
            // Fields the format leaves out: V1 repeats X, V2 repeats X and Y,
            // V3 leaves W as zero (the hardware leaves it undefined).
            if (u.vn == 0) {
                in[1] = in[2] = in[3] = in[0];
            } else if (u.vn == 1) {
                in[2] = in[0];
                in[3] = in[1];
            }
        }
    }

    uint32_t *dest = vu_.State().mem[addr & 0x3FF];
    uint32_t  cycle = wl_ == 0 ? 0 : n % wl_;
    uint32_t  mask_row = cycle > 3 ? 3 : cycle;
    for (int f = 0; f < 4; f++) {
        uint32_t m = u.masked ? (mask_ >> (mask_row * 8 + f * 2)) & 3 : 0;
        if (!has_data && m == 0) {
            m = 1; // a filled vector has no data: take the row register
        }
        switch (m) {
            case 0:
                if (mode_ == 1) {
                    dest[f] = in[f] + row_[f];
                } else if (mode_ == 2) {
                    row_[f] += in[f];
                    dest[f] = row_[f];
                } else {
                    dest[f] = in[f];
                }
                break;
            case 1:
                dest[f] = row_[f];
                break;
            case 2:
                dest[f] = col_[mask_row];
                break;
            default: // write-protected
                break;
        }
    }
}

} // namespace gfx
