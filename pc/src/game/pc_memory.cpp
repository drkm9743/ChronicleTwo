// Placement new into an mgCMemory allocation.
//
// Retail sizes most allocations in quadwords worked out for the EE, where a
// pointer takes 4 bytes: `new (memory->Alloc(5)) mgCVisualMDT`. The PC's
// types are larger, so placement new checks the size the compiler hands it
// against the allocation it was just given and, when that was the memory's
// latest, extends it to fit.

#include <cstddef>
#include <cstdio>

#include "common.h"
#include "mg_memory.hpp"

namespace {

struct LastAlloc {
    mgCMemory *memory;
    u_long128 *data;
    int        size; // quadwords
};

thread_local LastAlloc last;

void Widen(u_long128 *buffer, size_t bytes) {
    if (buffer == nullptr || buffer != last.data || last.memory == nullptr) {
        return;
    }
    int need = int((bytes + 15) / 16);
    if (need <= last.size) {
        return;
    }
    mgCMemory *memory = last.memory;
    int        extra = need - last.size;
    if (&memory->stack[memory->stack_used] != buffer + last.size) {
        return; // something was allocated after it
    }
    if (memory->stack_used + extra >= memory->stack_size) {
        printf("pc: no room to widen %d -> %d quadwords in %s\n", last.size, need, memory->name);
        return;
    }
    memory->stack_used += extra;
    last.size = need;
}

} // namespace

void pcNoteAlloc(mgCMemory *memory, u_long128 *data, int size) {
    last.memory = memory;
    last.data = data;
    last.size = size;
}

void pcStackOver(mgCMemory *memory, void *caller) {
    printf("pc: memory %p (\"%s\", buffer %p, %d quadwords) is full; Alloc called from %p\n", (void *) memory,
           memory->name, (void *) memory->stack, memory->stack_size, caller);
}

void *operator new(size_t size, u_long128 *buffer) {
    Widen(buffer, size);
    return buffer;
}

void *operator new[](size_t size, u_long128 *buffer) {
    Widen(buffer, size);
    return buffer;
}
