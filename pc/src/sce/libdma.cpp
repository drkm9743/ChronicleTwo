// libdma on pc/gfx: channel registers are plain memory the game may write,
// and a transfer runs to completion inside sceDmaSend, so sceDmaSync never
// waits. Channel 1 feeds VIF1 and channel 2 the GIF; channel 8 copies into
// the scratchpad.

#include "gfx.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

// Last: the SDK headers bring the game's own integer spellings.
#include "pc_hardware.h"

namespace {

const int kChannelCount = 10;
const int kChannelFromSpr = 8;
const int kChannelToSpr = 9;

sceDmaChan channels[kChannelCount];

int ChannelOf(const sceDmaChan *chan) {
    ptrdiff_t index = chan - channels;
    return index >= 0 && index < kChannelCount ? int(index) : -1;
}

/** Host pointer for a 32-bit address in a channel register; scratchpad addresses go to the scratchpad. */
uint8_t *Address(uint32_t addr, bool scratchpad) {
    return const_cast<uint8_t *>(gfx::GetMachine().memory_map.Translate(addr, scratchpad));
}

} // namespace

extern "C" {

sceDmaChan *sceDmaGetChan(int channel) {
    return channel >= 0 && channel < kChannelCount ? &channels[channel] : nullptr;
}

int sceDmaSend(sceDmaChan *chan, void *data) {
    int channel = ChannelOf(chan);
    if (channel < 0) {
        return -1;
    }
    // A source-chain transfer starting at the tag `data`.
    gfx::GetMachine().dma.SendChain(channel, data, chan->chcr.TTE != 0);
    chan->chcr.STR = 0;
    chan->qwc = 0;
    return 0;
}

int sceDmaSync(sceDmaChan *chan, int mode, int timeout) {
    (void) chan;
    (void) mode;
    (void) timeout;
    return 0;
}

int sceDmaReset(int mode) {
    (void) mode;
    std::memset(channels, 0, sizeof(channels));
    return 0;
}

int sceDmaGetEnv(sceDmaEnv *env) {
    std::memset(env, 0, sizeof(*env));
    return 0;
}

int sceDmaPutEnv(sceDmaEnv *env) {
    (void) env;
    return 0;
}

void pcDmaStart(sceDmaChan *chan) {
    int           channel = ChannelOf(chan);
    gfx::Machine &m = gfx::GetMachine();
    if (channel == kChannelFromSpr || channel == kChannelToSpr) {
        // Normal mode between memory and the scratchpad.
        size_t   bytes = size_t(chan->qwc) * 16;
        uint32_t spr = chan->sadr & (gfx::kScratchpadBytes - 1);
        bytes = std::min(bytes, size_t(gfx::kScratchpadBytes - spr));
        uint8_t *memory = Address(chan->madr, false);
        // Through the memory map, whose scratchpad is wherever the game writes it.
        uint8_t *scratchpad = Address(spr, true);
        if (channel == kChannelFromSpr) {
            std::memcpy(memory, scratchpad, bytes);
        } else {
            std::memcpy(scratchpad, memory, bytes);
        }
    } else if (channel >= 0) {
        if (chan->chcr.MOD == 1) {
            m.dma.SendChain(channel, Address(chan->tadr, false), chan->chcr.TTE != 0);
        } else {
            m.dma.SendNormal(channel, Address(chan->madr, false), chan->qwc);
        }
    }
    chan->chcr.STR = 0;
    chan->qwc = 0;
}

void pcGsWritePrivileged(u_int address, u_long value) {
    gfx::GetMachine().gs.WritePrivileged(address & 0x1FFF, value);
}

u_long pcGsReadPrivileged(u_int address) { return gfx::GetMachine().gs.ReadPrivileged(address & 0x1FFF); }

u_int pcTimer0Count() {
    // Mode 0x83 counts H-blanks: 15625 a second on PAL.
    static const auto start = std::chrono::steady_clock::now();
    auto              ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    return u_int(uint64_t(ns) * 15625 / 1000000000ull) & 0xFFFF;
}

} // extern "C"
