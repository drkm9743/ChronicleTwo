#include "iop.h"

#include <iterator>
#include <map>
#include <vector>

namespace pc {
namespace iop {

namespace {

/** Allocations are rounded to this, as the IOP's own allocator rounds them. */
const uint32_t kAlignment = 64;

struct Memory {
    std::vector<uint8_t>         bytes;
    std::map<uint32_t, uint32_t> used; /**< Offset of each allocation, to its size. */

    Memory() : bytes(kMemorySize) {}
};

Memory &GetMemory() {
    static Memory memory;
    return memory;
}

} // namespace

std::recursive_mutex &Lock() {
    static std::recursive_mutex lock;
    return lock;
}

void *Translate(uint32_t address, uint32_t size) {
    if (address < kMemoryBase || address - kMemoryBase > kMemorySize ||
        size > kMemorySize - (address - kMemoryBase)) {
        return NULL;
    }

    return &GetMemory().bytes[address - kMemoryBase];
}

uint32_t Alloc(uint32_t size) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    Memory                               &memory = GetMemory();
    uint32_t                              rounded = (size + kAlignment - 1) & ~(kAlignment - 1);
    uint32_t                              offset = 0;

    if (rounded == 0) {
        rounded = kAlignment;
    }

    // First fit: the gap before each allocation, then the space after the last.
    for (std::map<uint32_t, uint32_t>::iterator it = memory.used.begin(); it != memory.used.end(); ++it) {
        if (it->first - offset >= rounded) {
            break;
        }

        offset = it->first + it->second;
    }

    if (offset > kMemorySize || kMemorySize - offset < rounded) {
        return 0;
    }

    memory.used[offset] = rounded;
    return kMemoryBase + offset;
}

void Free(uint32_t address) {
    std::lock_guard<std::recursive_mutex> guard(Lock());

    if (address >= kMemoryBase) {
        GetMemory().used.erase(address - kMemoryBase);
    }
}

} // namespace iop
} // namespace pc
