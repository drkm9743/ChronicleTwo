// The DMA controller, as far as graphics needs it: source-chain transfers on
// channel 1 (to VIF1) and channel 2 (to the GIF), walked synchronously when
// the game starts them. Tag addresses are 31-bit EE addresses; a MemoryMap
// turns them into host pointers.

#pragma once

#include <cstddef>
#include <cstdint>

namespace gfx {

class Gif;
class Vif1;

/** Turns an address the game put in a DMA tag into a pointer this process can read. */
class MemoryMap {
public:
    virtual ~MemoryMap() = default;
    virtual const uint8_t *Translate(uint32_t addr, bool scratchpad) = 0;
};

/**
 * The map a PC build starts with: the scratchpad (tag SPR bit, or
 * 0x70000000-0x70003FFF) is a 16 KiB buffer, and every other address is the
 * host address itself, which holds while the executable keeps its data below
 * 2 GiB (/LARGEADDRESSAWARE:NO). `strip_mask` clears address bits the game
 * sets for the EE's uncached segments; it is 0 until the memory layout of the
 * PC build says which bits those can safely be.
 */
class DirectMemoryMap : public MemoryMap {
public:
    explicit DirectMemoryMap(uint8_t *scratchpad, uint32_t strip_mask = 0)
        : scratchpad_(scratchpad), strip_mask_(strip_mask) {}

    const uint8_t *Translate(uint32_t addr, bool scratchpad) override;

private:
    uint8_t *scratchpad_;
    uint32_t strip_mask_;
};

class Dma {
public:
    Dma(Vif1 &vif, Gif &gif, MemoryMap &map);

    void SetMemoryMap(MemoryMap &map) { map_ = &map; }

    /**
     * Runs a source-chain transfer starting with the tag at `tag`. On channel
     * 1 with `tte` the upper 64 bits of each tag go to the VIF before its data.
     * Returns the number of tags walked.
     */
    int SendChain(int channel, const void *tag, bool tte);

    /** Normal-mode transfer of `qwc` quadwords from `data`. */
    void SendNormal(int channel, const void *data, uint32_t qwc);

    /** Stops runaway chains (a CALL loop, a missing END). */
    static const int kMaxTags = 1 << 20;

private:
    void Deliver(int channel, const uint8_t *data, uint32_t qwc);

    Vif1      &vif_;
    Gif       &gif_;
    MemoryMap *map_;
};

} // namespace gfx
