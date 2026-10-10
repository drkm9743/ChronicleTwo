#include "../gif/gif.h"
#include "../vif/vif1.h"
#include "dma.h"

#include <cstdio>
#include <cstring>

namespace gfx {

namespace {

enum DmaTagId : uint32_t {
    TAG_REFE = 0,
    TAG_CNT = 1,
    TAG_NEXT = 2,
    TAG_REF = 3,
    TAG_REFS = 4,
    TAG_CALL = 5,
    TAG_RET = 6,
    TAG_END = 7,
};

} // namespace

const uint8_t *DirectMemoryMap::Translate(uint32_t addr, bool scratchpad) {
    if (scratchpad || (addr & 0xFFFFC000u) == 0x70000000u) {
        return scratchpad_ + (addr & 0x3FF0);
    }
    return reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(addr & ~strip_mask_));
}

Dma::Dma(Vif1 &vif, Gif &gif, MemoryMap &map) : vif_(vif), gif_(gif), map_(&map) {}

void Dma::Deliver(int channel, const uint8_t *data, uint32_t qwc) {
    if (qwc == 0) {
        return;
    }
    if (channel == 1) {
        // VIF codes are a word stream; copy so unaligned host data is fine.
        uint32_t words[64];
        size_t   total = static_cast<size_t>(qwc) * 4;
        for (size_t done = 0; done < total;) {
            size_t n = total - done < 64 ? total - done : 64;
            std::memcpy(words, data + done * 4, n * 4);
            vif_.Write(words, n);
            done += n;
        }
    } else if (channel == 2) {
        gif_.Write(GIF_PATH3, data, qwc);
    }
}

void Dma::SendNormal(int channel, const void *data, uint32_t qwc) {
    Deliver(channel, static_cast<const uint8_t *>(data), qwc);
}

int Dma::SendChain(int channel, const void *start, bool tte) {
    const uint8_t *tag = static_cast<const uint8_t *>(start);
    const uint8_t *stack[2];
    int            depth = 0;

    for (int count = 1; count <= kMaxTags; count++) {
        uint64_t lo;
        uint32_t upper[2];
        std::memcpy(&lo, tag, 8);
        std::memcpy(upper, tag + 8, 8);
        uint32_t qwc = lo & 0xFFFF;
        uint32_t id = (lo >> 28) & 7;
        uint32_t addr = static_cast<uint32_t>(lo >> 32) & 0x7FFFFFFF;
        bool     spr = (lo >> 63) & 1;

        if (channel == 1 && tte) {
            vif_.Write(upper, 2);
        }

        const uint8_t *after = tag + 16; // the quadword after the tag
        const uint8_t *next = nullptr;
        bool           end = false;

        switch (id) {
            case TAG_REFE:
                Deliver(channel, map_->Translate(addr, spr), qwc);
                end = true;
                break;
            case TAG_CNT:
                Deliver(channel, after, qwc);
                next = after + qwc * 16;
                break;
            case TAG_NEXT:
                Deliver(channel, after, qwc);
                next = map_->Translate(addr, spr);
                break;
            case TAG_REF:
            case TAG_REFS:
                Deliver(channel, map_->Translate(addr, spr), qwc);
                next = after;
                break;
            case TAG_CALL:
                Deliver(channel, after, qwc);
                if (depth == 2) {
                    std::fprintf(stderr, "gfx: DMA CALL nested deeper than 2, chain stopped\n");
                    return count;
                }
                stack[depth++] = after + qwc * 16;
                next = map_->Translate(addr, spr);
                break;
            case TAG_RET:
                Deliver(channel, after, qwc);
                if (depth == 0) {
                    end = true;
                } else {
                    next = stack[--depth];
                }
                break;
            case TAG_END:
                Deliver(channel, after, qwc);
                end = true;
                break;
        }

        if (end || next == nullptr) {
            return count;
        }
        tag = next;
    }
    std::fprintf(stderr, "gfx: DMA chain longer than %d tags, stopped\n", kMaxTags);
    return kMaxTags;
}

} // namespace gfx
