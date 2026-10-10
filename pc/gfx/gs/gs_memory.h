// The GS local memory: 4 MiB addressed through the page, block and column
// swizzles of each pixel format. Everything the CPU sees of GS memory (image
// transfers, CLUT loads, texture decoding, read-backs from render targets)
// goes through this class, so it is the one place the layouts live.

#pragma once

#include "gs_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace gfx {

/** A rectangle in pixels, half open: [x0, x1) x [y0, y1). */
struct GsRect {
    int x0, y0, x1, y1;

    bool Empty() const { return x1 <= x0 || y1 <= y0; }
};

/** The 512-entry CLUT buffer of the GS, 16 bits per entry as the hardware holds it. */
struct GsClut {
    uint16_t entries[512];
};

class GsMemory {
public:
    GsMemory();

    uint8_t *Data() { return data_.get(); }

    const uint8_t *Data() const { return data_.get(); }

    // Pixel addresses. `bp` is the base in 256-byte blocks and `bw` the buffer
    // width in units of 64 pixels, as BITBLTBUF, FRAME and TEX0 hold them.
    // Results are in units of the format's own pixel size and wrap at 4 MiB.
    static uint32_t Address32(uint32_t bp, uint32_t bw, int x, int y);  // 32-bit words
    static uint32_t Address32Z(uint32_t bp, uint32_t bw, int x, int y); // 32-bit words
    static uint32_t Address16(uint32_t bp, uint32_t bw, int x, int y);  // 16-bit units
    static uint32_t Address16S(uint32_t bp, uint32_t bw, int x, int y);
    static uint32_t Address16Z(uint32_t bp, uint32_t bw, int x, int y);
    static uint32_t Address16SZ(uint32_t bp, uint32_t bw, int x, int y);
    static uint32_t Address8(uint32_t bp, uint32_t bw, int x, int y); // bytes
    static uint32_t Address4(uint32_t bp, uint32_t bw, int x, int y); // nibbles

    /** Reads one pixel as stored: 32 bits for 32/24-bit formats, the index for 8/4-bit ones. */
    uint32_t ReadPixel(uint32_t psm, uint32_t bp, uint32_t bw, int x, int y) const;
    /** Writes one pixel, touching only the bits the format owns (24-bit formats keep the top byte). */
    void WritePixel(uint32_t psm, uint32_t bp, uint32_t bw, int x, int y, uint32_t value);

    /** Reads a pixel of a colour or depth format and returns it as 32-bit RGBA (A8B8G8R8 in memory order). */
    uint32_t ReadColor32(uint32_t psm, uint32_t bp, uint32_t bw, int x, int y) const;
    /** Writes RGBA8 into a colour or depth format, converting to 16 bits where the format needs it. */
    void WriteColor32(uint32_t psm, uint32_t bp, uint32_t bw, int x, int y, uint32_t rgba);

    /** Loads `count` CLUT entries from memory as TEX0's CBP, CPSM, CSM and CSA describe. */
    void LoadClut(GsClut &clut, uint32_t cbp, uint32_t cpsm, uint32_t csm, uint32_t csa,
                  uint32_t count, uint32_t texclut) const;

    /**
     * Decodes a texture to RGBA8. Indexed formats go through the CLUT; 24-bit
     * texels get alpha 0 and 16-bit ones 0x80/0 from their alpha bit, because
     * TEXA is applied when drawing, not here.
     */
    void DecodeTexture(uint32_t psm, uint32_t bp, uint32_t bw, int width, int height,
                       const GsClut &clut, uint32_t cpsm, uint32_t csa, uint32_t *out) const;

    // Pages, for invalidation. A page is 8 KiB; every write bumps a stamp.

    /** Calls fn(page) for every page a rectangle of a buffer touches, once each. */
    template <typename Fn>
    static void ForEachPage(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect, Fn fn);

    void MarkWritten(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect);

    uint64_t PageStamp(uint32_t page) const { return page_stamp_[page % kGsPageCount]; }

    /** Highest write stamp among the pages a rectangle touches. */
    uint64_t RegionStamp(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) const;

    uint64_t NextStamp() { return ++stamp_; }

private:
    std::unique_ptr<uint8_t[]> data_;
    uint64_t                   page_stamp_[kGsPageCount] = {};
    uint64_t                   stamp_ = 0;
};

/** Number of pages a buffer row spans for a format: bw is in 64-pixel units. */
inline uint32_t PagesPerRow(uint32_t psm, uint32_t bw) {
    uint32_t w = bw * 64;
    uint32_t pw = static_cast<uint32_t>(PsmPageWidth(psm));
    return w == 0 ? 1 : (w + pw - 1) / pw;
}

template <typename Fn>
void GsMemory::ForEachPage(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect, Fn fn) {
    if (rect.Empty()) {
        return;
    }
    // A buffer that does not start on a page boundary straddles two pages per
    // page-sized tile, so both are reported.
    int      pw = PsmPageWidth(psm);
    int      ph = PsmPageHeight(psm);
    uint32_t per_row = PagesPerRow(psm, bw);
    uint32_t first = bp / 32;
    bool     straddle = (bp % 32) != 0;
    uint32_t seen[kGsPageCount / 32] = {};
    for (int py = rect.y0 / ph; py <= (rect.y1 - 1) / ph; py++) {
        for (int px = rect.x0 / pw; px <= (rect.x1 - 1) / pw; px++) {
            uint32_t page = (first + py * per_row + px) % kGsPageCount;
            for (int k = 0; k < (straddle ? 2 : 1); k++) {
                uint32_t p = (page + k) % kGsPageCount;
                if (!(seen[p / 32] & (1u << (p % 32)))) {
                    seen[p / 32] |= 1u << (p % 32);
                    fn(p);
                }
            }
        }
    }
}

} // namespace gfx
