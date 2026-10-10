// libpkt: DMA chains for VIF1 and the GIF built in a buffer the caller owns.
// Each packet keeps one DMA tag open (its QWC is written when the next tag
// starts or the packet is terminated) and, for VIF1, at most one DIRECT code
// and one GIF tag whose lengths are written when they close.
//
// DMA tags hold 32-bit addresses: CALL and REF targets must lie in memory
// the DMA MemoryMap can reach (see pc/gfx/dma/dma.h).

#include <libpkt.h>

#include <cstdint>
#include <cstring>

namespace {

const uint32_t kTagCnt = 1u << 28;
const uint32_t kTagRef = 3u << 28;
const uint32_t kTagCall = 5u << 28;
const uint32_t kTagEnd = 7u << 28;
const uint32_t kVifDirect = 0x50u << 24;

uint32_t Address32(const void *p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

/** Pads with zero words (VIF NOPs) up to a quadword boundary. */
u_int *AlignQuad(u_int *p) {
    while (reinterpret_cast<uintptr_t>(p) & 0xF) {
        *p++ = 0;
    }
    return p;
}

/** Writes the QWC of the tag at `tag` for the data up to `end` and returns the aligned end. */
u_int *CloseDma(u_int *tag, u_int *end) {
    end = AlignQuad(end);
    if (tag) {
        uint32_t qwc = uint32_t((end - tag) / 4 - 1);
        tag[0] = (tag[0] & 0xFFFF0000u) | (qwc & 0xFFFF);
    }
    return end;
}

/** Sets NLOOP of the GIF tag at `tag` from the quadwords after it. */
void CloseGif(u_int *tag, u_int *end) {
    uint32_t qwc = uint32_t((end - tag) / 4 - 1);
    uint64_t lo;
    std::memcpy(&lo, tag, 8);
    uint32_t flg = uint32_t(lo >> 58) & 3;
    uint32_t nreg = uint32_t(lo >> 60) & 0xF;
    if (nreg == 0) {
        nreg = 16;
    }
    // PACKED: NREG quadwords a loop; REGLIST: NREG doublewords; IMAGE: one quadword.
    uint32_t nloop = flg == 0 ? qwc / nreg : flg == 1 ? qwc * 2 / nreg
                                                      : qwc;
    lo = (lo & ~uint64_t(0x7FFF)) | (nloop & 0x7FFF);
    std::memcpy(tag, &lo, 8);
}

/** Starts a DMA tag at pCurrent; VIF codes go in its upper half. */
void OpenVifDma(sceVif1Packet *pk, uint32_t id_irq, uint32_t addr) {
    pk->pCurrent = CloseDma(pk->pOpenDma, pk->pCurrent);
    u_int *tag = pk->pCurrent;
    tag[0] = id_irq;
    tag[1] = addr;
    pk->pOpenDma = tag;
    pk->pCurrent = tag + 2;
}

/** The GIF packet keeps its open DMA tag as a word offset from pBase (+1; 0 is none). */
u_int *GifOpenDma(sceGifPacket *pk) {
    return pk->reserved ? reinterpret_cast<u_int *>(pk->pBase) + (pk->reserved - 1) : nullptr;
}

void OpenGifDma(sceGifPacket *pk, uint32_t id_irq, uint32_t addr, bool keep_open) {
    u_int *cur = CloseDma(GifOpenDma(pk), reinterpret_cast<u_int *>(pk->pCurrent));
    cur[0] = id_irq;
    cur[1] = addr;
    cur[2] = 0;
    cur[3] = 0;
    pk->reserved = keep_open ? u_int(cur - reinterpret_cast<u_int *>(pk->pBase)) + 1 : 0;
    pk->pCurrent = reinterpret_cast<u_long128 *>(cur + 4);
}

uint32_t IrqPce(int irq, int pce) { return (uint32_t(irq & 1) << 31) | (uint32_t(pce & 3) << 26); }

} // namespace

extern "C" {

//
// VIF1
//

void sceVif1PkInit(sceVif1Packet *packet, u_int *base) {
    std::memset(packet, 0, sizeof(*packet));
    packet->pBase = base;
    sceVif1PkReset(packet);
}

void sceVif1PkReset(sceVif1Packet *packet) {
    packet->pCurrent = packet->pBase;
    packet->pOpenTag = nullptr;
    packet->pOpenDma = nullptr;
    packet->reserved[0] = nullptr;
}

u_int *sceVif1PkTerminate(sceVif1Packet *packet) {
    packet->pCurrent = CloseDma(packet->pOpenDma, packet->pCurrent);
    packet->pOpenDma = nullptr;
    return packet->pCurrent;
}

u_int *sceVif1PkEnd(sceVif1Packet *packet, u_int next) {
    OpenVifDma(packet, kTagEnd, 0);
    u_int *tag = packet->pOpenDma;
    // The upper half: a VIF NOP and the code the caller passes.
    *packet->pCurrent++ = 0;
    *packet->pCurrent++ = next;
    return tag;
}

void sceVif1PkReserve(sceVif1Packet *packet, int count) { packet->pCurrent += count; }

void sceVif1PkCnt(sceVif1Packet *packet, int irq) { OpenVifDma(packet, kTagCnt | IrqPce(irq, 0), 0); }

void sceVif1PkCall(sceVif1Packet *packet, u_long128 *address, u_int count) {
    // The callee runs to its RET; the next tag follows this one.
    OpenVifDma(packet, kTagCall | (count & 0xFFFF), Address32(address));
    *packet->pCurrent++ = 0;
    *packet->pCurrent++ = 0;
    packet->pOpenDma = nullptr;
}

void sceVif1PkOpenDirectCode(sceVif1Packet *packet, int irq) {
    // DIRECT is the last word of a quadword so the GIF data after it is aligned.
    while ((reinterpret_cast<uintptr_t>(packet->pCurrent) & 0xF) != 0xC) {
        *packet->pCurrent++ = 0;
    }
    packet->pOpenTag = packet->pCurrent;
    *packet->pCurrent++ = kVifDirect | (uint32_t(irq & 1) << 31);
}

void sceVif1PkCloseDirectCode(sceVif1Packet *packet) {
    if (packet->pOpenTag == nullptr) {
        return;
    }
    packet->pCurrent = AlignQuad(packet->pCurrent);
    uint32_t qwc = uint32_t((packet->pCurrent - (packet->pOpenTag + 1)) / 4);
    *packet->pOpenTag = (*packet->pOpenTag & 0xFFFF0000u) | (qwc & 0xFFFF);
    packet->pOpenTag = nullptr;
}

void sceVif1PkOpenGifTag(sceVif1Packet *packet, u_long128 giftag) {
    packet->pCurrent = AlignQuad(packet->pCurrent);
    std::memcpy(packet->pCurrent, &giftag, 16);
    packet->reserved[0] = packet->pCurrent;
    packet->pCurrent += 4;
}

void sceVif1PkCloseGifTag(sceVif1Packet *packet) {
    if (packet->reserved[0] == nullptr) {
        return;
    }
    CloseGif(packet->reserved[0], AlignQuad(packet->pCurrent));
    packet->reserved[0] = nullptr;
}

void sceVif1PkAddGsAD(sceVif1Packet *packet, int reg, u_long data) {
    uint64_t qw[2] = {data, uint64_t(reg)};
    std::memcpy(packet->pCurrent, qw, 16);
    packet->pCurrent += 4;
}

//
// GIF
//

void sceGifPkInit(sceGifPacket *packet, u_long128 *base) {
    std::memset(packet, 0, sizeof(*packet));
    packet->pBase = base;
    sceGifPkReset(packet);
}

void sceGifPkReset(sceGifPacket *packet) {
    packet->pCurrent = packet->pBase;
    packet->pOpenTag = nullptr;
    packet->reserved = 0;
}

u_int *sceGifPkTerminate(sceGifPacket *packet) {
    u_int *end = CloseDma(GifOpenDma(packet), reinterpret_cast<u_int *>(packet->pCurrent));
    packet->reserved = 0;
    packet->pCurrent = reinterpret_cast<u_long128 *>(end);
    return end;
}

u_int *sceGifPkEnd(sceGifPacket *packet, int pce, int irq, int pad) {
    (void) pad;
    OpenGifDma(packet, kTagEnd | IrqPce(irq, pce), 0, true);
    return GifOpenDma(packet);
}

void sceGifPkCnt(sceGifPacket *packet, int pce, int irq, int pad) {
    (void) pad;
    OpenGifDma(packet, kTagCnt | IrqPce(irq, pce), 0, true);
}

u_int *sceGifPkReserve(sceGifPacket *packet, int count) {
    u_int *start = reinterpret_cast<u_int *>(packet->pCurrent);
    packet->pCurrent = reinterpret_cast<u_long128 *>(start + count);
    return start;
}

void sceGifPkRef(sceGifPacket *packet, u_long128 *address, int count, int pce, int irq, int pad) {
    (void) pad;
    OpenGifDma(packet, kTagRef | IrqPce(irq, pce) | (uint32_t(count) & 0xFFFF), Address32(address), false);
}

void sceGifPkOpenGifTag(sceGifPacket *packet, u_long128 giftag) {
    *packet->pCurrent = giftag;
    packet->pOpenTag = packet->pCurrent;
    packet->pCurrent++;
}

void sceGifPkCloseGifTag(sceGifPacket *packet) {
    if (packet->pOpenTag == nullptr) {
        return;
    }
    CloseGif(reinterpret_cast<u_int *>(packet->pOpenTag), reinterpret_cast<u_int *>(packet->pCurrent));
    packet->pOpenTag = nullptr;
}

void sceGifPkAddGsAD(sceGifPacket *packet, int reg, u_long data) {
    uint64_t qw[2] = {data, uint64_t(reg)};
    std::memcpy(packet->pCurrent, qw, 16);
    packet->pCurrent++;
}

} // extern "C"
