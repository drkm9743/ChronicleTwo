// The EE registers the game reads and writes by address (common.h's
// pcHwRead32 and friends): timer 0, which mglib runs in H-blank mode to time
// its frames, the GS privileged registers, and DMAC and INTC registers kept
// as plain values.

#include "kernel.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// Last: the SDK headers bring the game's own integer spellings.
#include "../sce/pc_hardware.h"

namespace {

const u_int kTimer0Count = 0x10000000;
const u_int kTimer0Mode = 0x10000010;
const u_int kGsPrivileged = 0x12000000;
const u_int kGsPrivilegedEnd = 0x12002000;

// H-blanks a second: 15625 on PAL.
const uint64_t kHBlankRate = 15625;

struct Timer0 {
    std::chrono::steady_clock::time_point base = std::chrono::steady_clock::now();
    u_int                                 base_count = 0;
    u_int                                 mode = 0;
};

Timer0 timer0;

std::mutex                         registers_lock;
std::unordered_map<u_int, u_long> registers;

u_int Timer0Count() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - timer0.base).count();
    return (timer0.base_count + u_int(uint64_t(ns) * kHBlankRate / 1000000000ull)) & 0xFFFF;
}

} // namespace

extern "C" {

u_int pcHwRead32(u_int address) {
    if (address == kTimer0Count) {
        return Timer0Count();
    }
    if (address == kTimer0Mode) {
        return timer0.mode;
    }
    return u_int(pcHwRead64(address));
}

void pcHwWrite32(u_int address, u_int value) {
    if (address == kTimer0Count) {
        timer0.base = std::chrono::steady_clock::now();
        timer0.base_count = value & 0xFFFF;
        return;
    }
    if (address == kTimer0Mode) {
        timer0.mode = value;
        return;
    }
    pcHwWrite64(address, value);
}

u_long pcHwRead64(u_int address) {
    if (address >= kGsPrivileged && address < kGsPrivilegedEnd) {
        return pcGsReadPrivileged(address);
    }
    if (address == kTimer0Count || address == kTimer0Mode) {
        return pcHwRead32(address);
    }
    std::lock_guard<std::mutex> lock(registers_lock);
    auto                        it = registers.find(address);
    return it == registers.end() ? 0 : it->second;
}

void pcHwWrite64(u_int address, u_long value) {
    if (address >= kGsPrivileged && address < kGsPrivilegedEnd) {
        pcGsWritePrivileged(address, value);
        return;
    }
    if (address == kTimer0Count || address == kTimer0Mode) {
        pcHwWrite32(address, u_int(value));
        return;
    }
    std::lock_guard<std::mutex> lock(registers_lock);
    registers[address] = value;
}

} // extern "C"
