#include "gfx.h"

#include <cstring>

namespace gfx {

Machine::Machine()
    : gs(memory), gif(gs), vu1(gif), vif1(vu1, gif), memory_map(nullptr), dma(vif1, gif, memory_map) {
    std::memset(scratchpad, 0, sizeof(scratchpad));
    memory_map = DirectMemoryMap(scratchpad);
}

void Machine::Reset() {
    gs.Flush();
    gs.Reset();
    gif.Reset();
    vif1.Reset();
}

Machine &GetMachine() {
    static Machine *machine = new Machine();
    return *machine;
}

} // namespace gfx
