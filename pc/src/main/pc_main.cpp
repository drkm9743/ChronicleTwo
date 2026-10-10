// The PC executable's entry point: sets up what the PS2 had before the game's
// main ran (the scratchpad, the EE kernel, the disc), takes the data the
// game needs from the retail executable on the disc, and starts the game.
//
//   chronicletwo [disc]
//
// `disc` is the ISO image or the folder holding the extracted disc; without
// it, DC2_DISC or "disc" beside the executable (pc/src/host/host_paths.h).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../disc/disc.h"
#include "../ee/kernel.h"
#include "../host/host_paths.h"
#include "gfx.h"
#include "vu1/dc2_programs.h"

int ps2Main();
int pcLoadRetailData();
void pcRetailVuPrograms(const void *packets[5]);

namespace {

const uint32_t kScratchpadAddress = 0x70000000;
const char     kExecutable[] = "SCES_511.90";

/**
 * The EE scratchpad, at the address the game writes it through. The game
 * builds packets there by address (GetScrPad returns 0x70000000), and with
 * /LARGEADDRESSAWARE:NO that address is ours to take.
 */
void MapScratchpad() {
    void *spr = VirtualAlloc(reinterpret_cast<void *>(uintptr_t(kScratchpadAddress)), gfx::kScratchpadBytes,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (spr != reinterpret_cast<void *>(uintptr_t(kScratchpadAddress))) {
        std::fprintf(stderr, "chronicletwo: cannot map the scratchpad at 0x%08X (%lu)\n", kScratchpadAddress,
                     GetLastError());
        std::exit(1);
    }
    // DMA tags reach it there too, by address or by the SPR bit.
    gfx::GetMachine().memory_map = gfx::DirectMemoryMap(static_cast<uint8_t *>(spr));
}

/** The game keeps pointers in 32-bit integers; everything it can point at must sit below 2 GiB. */
void CheckAddressSpace() {
    void   *heap = std::malloc(64);
    int     stack = 0;
    uint64_t highest = std::max<uint64_t>(uint64_t(uintptr_t(heap)), uint64_t(uintptr_t(&stack)));
    highest = std::max<uint64_t>(highest, uint64_t(uintptr_t(&CheckAddressSpace)));
    std::free(heap);
    if (highest >= 0x80000000ull) {
        std::fprintf(stderr,
                     "chronicletwo: memory above 2 GiB (0x%llx); the executable must be linked with "
                     "/LARGEADDRESSAWARE:NO\n",
                     (unsigned long long) highest);
        std::exit(1);
    }
}

struct Executable {
    std::vector<uint8_t> bytes;
    bool                 loaded = false;
};

Executable &Retail() {
    static Executable exe;
    return exe;
}

bool LoadExecutable() {
    pc::disc::File file;
    if (!pc::disc::Find(kExecutable, &file) || file.size < 0x34) {
        return false;
    }
    Executable &exe = Retail();
    exe.bytes.resize(file.size);
    if (!pc::disc::Read(uint64_t(file.lsn) * pc::disc::kSectorSize, exe.bytes.data(), file.size)) {
        return false;
    }
    const uint8_t *b = exe.bytes.data();
    if (std::memcmp(b, "\x7F" "ELF", 4) != 0) {
        return false;
    }
    exe.loaded = true;
    return true;
}

uint32_t Read32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

uint16_t Read16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }

} // namespace

extern "C" int pcReadRetailExecutable(unsigned int address, void *buffer, unsigned int size) {
    Executable &exe = Retail();
    if (!exe.loaded) {
        return 0;
    }
    const uint8_t *b = exe.bytes.data();
    uint32_t       phoff = Read32(b + 0x1C);
    uint16_t       phentsize = Read16(b + 0x2A), phnum = Read16(b + 0x2C);
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = b + phoff + size_t(i) * phentsize;
        if (ph + 0x20 > b + exe.bytes.size() || Read32(ph) != 1) { // PT_LOAD
            continue;
        }
        uint32_t offset = Read32(ph + 4), vaddr = Read32(ph + 8), filesz = Read32(ph + 16);
        if (address >= vaddr && address + size <= vaddr + filesz && size_t(offset) + filesz <= exe.bytes.size()) {
            std::memcpy(buffer, b + offset + (address - vaddr), size);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1) {
        pc::SetDiscPath(pc::PathFromUtf8(argv[1]));
    }
    CheckAddressSpace();
    MapScratchpad();

    if (!pc::disc::Mount()) {
        std::fprintf(stderr, "chronicletwo: no disc at %s (give the ISO or the extracted folder, or set DC2_DISC)\n",
                     pc::Utf8FromPath(pc::DiscPath()).c_str());
        return 1;
    }
    if (!LoadExecutable() || !pcLoadRetailData()) {
        std::fprintf(stderr, "chronicletwo: cannot read %s from the disc; is it the PAL Dark Chronicle?\n",
                     kExecutable);
        return 1;
    }
    const void *vu[5];
    pcRetailVuPrograms(vu);
    gfx::Dc2ProgramPackets packets;
    packets.progmain = vu[0];
    packets.model = vu[1];
    packets.shadow = vu[2];
    packets.sprite3d = vu[3];
    packets.water = vu[4];
    if (gfx::RegisterDc2Programs(gfx::GetMachine().vu1, packets) != 5) {
        std::fprintf(stderr, "chronicletwo: the VU1 programs on the disc are not the ones pc/vu1 translates\n");
        return 1;
    }
    Retail().bytes.clear();
    Retail().bytes.shrink_to_fit();
    Retail().loaded = false;

    pc::ee::KernelStart(true);
    int status = ps2Main();
    pc::ee::KernelShutdown();
    return status;
}
