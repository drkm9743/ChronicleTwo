// Replays the last frame the game sent in a PCSX2 save state through the
// whole PC graphics path: the VIF1 DMA chain from EE memory, VIF1, the C++
// VU1 microprograms, PATH1 into the GS and the Vulkan renderer, and writes
// the frame buffer it drew. A way to see real game data on the renderer
// before the game itself runs on the PC.
//
//   gfx_replay SCES_511.90 STATE_DIR OUT.ppm [--clear]
//
// --clear paints the game's two 512x480 frame buffers magenta first, so
// anything the frame did not draw shows.
//
// STATE_DIR holds the save state's eeMemory.bin, Scratchpad.bin,
// eeHwRegs.bin, vu1Memory.bin, vu1MicroMem.bin and GS.bin, unpacked from the
// .p2s (a zip with zstd entries). GS memory comes from GS.bin, so textures
// the game uploaded earlier are there; the frame buffer starts as the state
// left it.

#include "gfx.h"
#include "vu1/dc2_programs.h"
#include "vulkan/vk_context.h"
#include "vulkan/vk_renderer.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace gfx;

namespace {

// PAL executable: ELF virtual address minus file offset.
const uint32_t kElfBias = 0xFFF80;
const size_t   kElfSize = 4054516;

// Upload packets of the microprograms (ps2/config/pal/main.symbols.txt).
const uint32_t kVuProgMain = 0x326CC0;
const uint32_t kVuProg0 = 0x326F90;
const uint32_t kVuProgSdw = 0x326350;
const uint32_t kVuProg3dsp = 0x325C80;
const uint32_t kVuProgWtr = 0x329FD0;

// mglib's two VIF1 packets (sceVif1Packet, 0x20 bytes each).
const uint32_t kVifPackets = 0x398580;

// GS.bin of a version 9 GS freeze: registers, then the 4 MiB of local memory.
const size_t kGsFreezeMemoryOffset = 429;

std::vector<uint8_t> ReadFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

uint32_t Word(const std::vector<uint8_t> &data, size_t offset) {
    uint32_t v = 0;
    if (offset + 4 <= data.size()) {
        std::memcpy(&v, &data[offset], 4);
    }
    return v;
}

/** EE addresses in the save state's main memory and scratchpad. */
class SaveStateMap : public MemoryMap {
public:
    SaveStateMap(std::vector<uint8_t> &ram, std::vector<uint8_t> &spr) : ram_(ram), spr_(spr) {}

    const uint8_t *Translate(uint32_t addr, bool scratchpad) override {
        if (scratchpad || (addr & 0xF0000000u) == 0x70000000u) {
            return &spr_[addr & 0x3FF0];
        }
        return &ram_[addr & 0x1FFFFF0];
    }

private:
    std::vector<uint8_t> &ram_;
    std::vector<uint8_t> &spr_;
};

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: gfx_replay ELF STATE_DIR OUT.ppm\n");
        return 2;
    }
    const std::string    dir = argv[2];
    std::vector<uint8_t> elf = ReadFile(argv[1]);
    std::vector<uint8_t> ram = ReadFile(dir + "/eeMemory.bin");
    std::vector<uint8_t> spr = ReadFile(dir + "/Scratchpad.bin");
    std::vector<uint8_t> hw = ReadFile(dir + "/eeHwRegs.bin");
    std::vector<uint8_t> vu_data = ReadFile(dir + "/vu1Memory.bin");
    std::vector<uint8_t> vu_micro = ReadFile(dir + "/vu1MicroMem.bin");
    std::vector<uint8_t> gs = ReadFile(dir + "/GS.bin");
    if (elf.size() != kElfSize || ram.size() != (32u << 20) || spr.size() != kScratchpadBytes ||
        hw.size() != 0x10000 || vu_data.size() != 16384 || vu_micro.size() != 16384 ||
        gs.size() < kGsFreezeMemoryOffset + kGsMemoryBytes) {
        std::fprintf(stderr, "gfx_replay: missing or unexpected input files\n");
        return 2;
    }

    VulkanContext ctx;
    std::string   error;
    if (!ctx.Init(VulkanContextOptions(), &error)) {
        std::fprintf(stderr, "gfx_replay: %s\n", error.c_str());
        return 1;
    }
    Machine       &m = GetMachine();
    VulkanRenderer renderer(ctx, m.memory);
    if (!renderer.Init(&error)) {
        std::fprintf(stderr, "gfx_replay: %s\n", error.c_str());
        return 1;
    }
    m.SetRenderer(&renderer);
    std::memcpy(m.memory.Data(), &gs[kGsFreezeMemoryOffset], kGsMemoryBytes);
    if (argc > 4 && std::strcmp(argv[4], "--clear") == 0) {
        for (uint32_t fbp : {0u, 0xF00u}) {
            for (int y = 0; y < 480; y++) {
                for (int x = 0; x < 512; x++) {
                    m.memory.WritePixel(PSMCT32, fbp, 8, x, y, 0x80FF00FF);
                }
            }
        }
    }

    SaveStateMap map(ram, spr);
    m.dma.SetMemoryMap(map);

    // VU1 as the state left it, and the C++ programs for what the game uploads.
    std::memcpy(m.vu1.State().mem, vu_data.data(), vu_data.size());
    m.vu1.LoadMicro(0, reinterpret_cast<const uint64_t *>(vu_micro.data()), 2048);
    Dc2ProgramPackets packets;
    packets.progmain = &elf[kVuProgMain - kElfBias];
    packets.model = &elf[kVuProg0 - kElfBias];
    packets.shadow = &elf[kVuProgSdw - kElfBias];
    packets.sprite3d = &elf[kVuProg3dsp - kElfBias];
    packets.water = &elf[kVuProgWtr - kElfBias];
    int registered = RegisterDc2Programs(m.vu1, packets);

    // The frame sent last: the VIF1 packet whose end is in D1_MADR.
    uint32_t madr = Word(hw, 0x9010);
    uint32_t base = 0;
    for (int i = 0; i < 2; i++) {
        if (Word(ram, (kVifPackets + 0x20 * i) & 0x1FFFFFF) == madr) {
            base = Word(ram, (kVifPackets + 0x20 * i + 4) & 0x1FFFFFF);
        }
    }
    if (base == 0) {
        std::fprintf(stderr, "gfx_replay: no VIF1 packet ends at D1_MADR %08x\n", madr);
        return 1;
    }
    int tags = m.dma.SendChain(1, map.Translate(base, false), true);
    m.gs.Flush();
    renderer.SyncAll();

    // The buffer the frame drew into.
    uint64_t frame = m.gs.Reg(GS_FRAME_1);
    uint32_t fbp = Bits(frame, 0, 9) * 32, fbw = Bits(frame, 16, 6), psm = Bits(frame, 24, 6);
    int      width = int(fbw) * 64, height = int(Bits(m.gs.Reg(GS_SCISSOR_1), 48, 11)) + 1;
    if (width <= 0 || height <= 1) {
        width = 640;
        height = 448;
        fbw = 10;
    }
    FILE *out = std::fopen(argv[3], "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "gfx_replay: cannot write %s\n", argv[3]);
        return 1;
    }
    std::fprintf(out, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t c = m.memory.ReadColor32(IsColorFormat(psm) ? psm : uint32_t(PSMCT32), fbp, fbw, x, y);
            uint8_t  rgb[3] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16)};
            std::fwrite(rgb, 1, 3, out);
        }
    }
    std::fclose(out);

    const VulkanRenderer::Stats &s = renderer.GetStats();
    std::printf("gfx_replay: %d programs registered, %d DMA tags, %llu draws, %llu vertices, %llu textures, "
                "%zu unhandled MSCAL addresses; frame buffer 0x%x (%dx%d, psm 0x%x)\n",
                registered, tags, (unsigned long long) s.draws, (unsigned long long) s.vertices,
                (unsigned long long) s.texture_uploads, m.vu1.Unhandled().size(), fbp, width, height, psm);
    m.SetRenderer(nullptr);
    return 0;
}
