#pragma once

// The IOP as the PC build sees it: a block of memory the game addresses with
// small integers, as it addresses the PS2's I/O processor, and the RPC
// servers the game's IOP modules provided, run in the same process.
//
// The game never reads IOP memory itself. It allocates it with
// sceSifAllocIopHeap, fills it with sceSifSetDma and hands the addresses to
// the servers, so an IOP address only has to mean something here. Keeping
// the addresses small matters: the game stores them in ints and masks them to
// 24 bits.

#include <cstdint>
#include <mutex>

namespace pc {
namespace iop {

/** The lowest IOP address handed out; zero stays free to mean "no memory". */
const uint32_t kMemoryBase = 0x00100000;

/** How much IOP memory there is. The PS2 had 2 MB for modules and buffers together. */
const uint32_t kMemorySize = 0x00400000;

/** Guards everything that runs "on the IOP": the RPC servers, the sound processor and its mixer. */
std::recursive_mutex &Lock();

/** Gives the host memory behind an IOP address range, or NULL when it is not all IOP memory. */
void *Translate(uint32_t address, uint32_t size);

/** Allocates IOP memory and gives its address, or zero when there is none left. */
uint32_t Alloc(uint32_t size);

/** Frees IOP memory from Alloc; other addresses are ignored. */
void Free(uint32_t address);

/** The IOP address the game holds in a pointer. */
inline uint32_t AddressOf(const void *pointer) {
    return (uint32_t) (uintptr_t) pointer;
}

/** The pointer the game holds for an IOP address. */
inline void *PointerOf(uint32_t address) {
    return (void *) (uintptr_t) address;
}

/**
 * Handles one call to an RPC server: `function` is the command word,
 * `data` the `size` bytes the client sent. Gives the reply buffer, of which
 * the client copies as much as it asked for, or NULL for no reply.
 */
typedef void *(*RpcHandler)(unsigned int function, void *data, int size);

/** The server bound to an RPC number, or NULL when no module provides it. */
RpcHandler FindServer(unsigned int number);

} // namespace iop
} // namespace pc
