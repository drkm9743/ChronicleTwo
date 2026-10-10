// Tests for the GPU-free half of pc/gfx: GS memory layouts and transfers,
// CLUT loads, GIF packets turned into primitives, VIF1 unpacking and
// microprogram dispatch, and DMA chains.

#include "gfx.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <vector>

using namespace gfx;

namespace {

int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                                                       \
    do {                                                                                                     \
        auto va = (a);                                                                                       \
        auto vb = (b);                                                                                       \
        if (!(va == vb)) {                                                                                   \
            std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long) va, \
                        (long long) vb);                                                                     \
            failures++;                                                                                      \
        }                                                                                                    \
    } while (0)

/** Records what reaches the renderer. */
class RecordingRenderer : public IGsRenderer {
public:
    struct Batch {
        GsDrawState           state;
        std::vector<GsVertex> vertices;
    };

    std::vector<Batch>  batches;
    std::vector<GsRect> written;
    int                 vsyncs = 0;

    void Draw(const GsDrawState &state, const GsVertex *v, size_t count, const GsClut &) override {
        batches.push_back(Batch{state, std::vector<GsVertex>(v, v + count)});
    }

    void SyncToMemory(uint32_t, uint32_t, uint32_t, const GsRect &) override {}

    void PrepareWrite(uint32_t, uint32_t, uint32_t, const GsRect &) override {}

    void MemoryWritten(uint32_t, uint32_t, uint32_t, const GsRect &r) override { written.push_back(r); }

    void VSync(const GsDisplayState &) override { vsyncs++; }
};

uint64_t GifTag(uint32_t nloop, bool eop, bool pre, uint32_t prim, uint32_t flg, uint32_t nreg) {
    return nloop | (uint64_t(eop) << 15) | (uint64_t(pre) << 46) | (uint64_t(prim) << 47) | (uint64_t(flg) << 58) |
           (uint64_t(nreg) << 60);
}

struct Qw {
    uint32_t w[4];
};

Qw MakeQw(uint64_t lo, uint64_t hi) {
    Qw q;
    q.w[0] = static_cast<uint32_t>(lo);
    q.w[1] = static_cast<uint32_t>(lo >> 32);
    q.w[2] = static_cast<uint32_t>(hi);
    q.w[3] = static_cast<uint32_t>(hi >> 32);
    return q;
}

Qw AD(uint32_t reg, uint64_t value) { return MakeQw(value, reg); }

uint64_t Xyz(int x, int y, uint32_t z) { return uint64_t(x) | (uint64_t(y) << 16) | (uint64_t(z) << 32); }

void TestAddressing() {
    // Each format maps one page of pixels onto 8 KiB without collisions.
    struct Case {
        uint32_t psm;
        int      w, h;
        uint32_t units;
    } cases[] = {
        {PSMCT32,  64,  32,  2048 },
        {PSMZ32,   64,  32,  2048 },
        {PSMCT16,  64,  64,  4096 },
        {PSMCT16S, 64,  64,  4096 },
        {PSMZ16,   64,  64,  4096 },
        {PSMZ16S,  64,  64,  4096 },
        {PSMT8,    128, 64,  8192 },
        {PSMT4,    128, 128, 16384},
    };

    for (const Case &c : cases) {
        std::set<uint32_t> seen;
        for (int y = 0; y < c.h; y++) {
            for (int x = 0; x < c.w; x++) {
                uint32_t a;
                switch (c.psm) {
                    case PSMCT32:
                        a = GsMemory::Address32(0, 2, x, y);
                        break;
                    case PSMZ32:
                        a = GsMemory::Address32Z(0, 2, x, y);
                        break;
                    case PSMCT16:
                        a = GsMemory::Address16(0, 2, x, y);
                        break;
                    case PSMCT16S:
                        a = GsMemory::Address16S(0, 2, x, y);
                        break;
                    case PSMZ16:
                        a = GsMemory::Address16Z(0, 2, x, y);
                        break;
                    case PSMZ16S:
                        a = GsMemory::Address16SZ(0, 2, x, y);
                        break;
                    case PSMT8:
                        a = GsMemory::Address8(0, 2, x, y);
                        break;
                    default:
                        a = GsMemory::Address4(0, 2, x, y);
                        break;
                }
                CHECK(a < c.units);
                seen.insert(a);
            }
        }
        CHECK_EQ(seen.size(), static_cast<size_t>(c.units));
    }

    // Known positions: the first column of a PSMCT32 block, the next block, the next page.
    CHECK_EQ(GsMemory::Address32(0, 10, 0, 0), 0u);
    CHECK_EQ(GsMemory::Address32(0, 10, 1, 0), 1u);
    CHECK_EQ(GsMemory::Address32(0, 10, 0, 1), 2u);
    CHECK_EQ(GsMemory::Address32(0, 10, 2, 0), 4u);
    CHECK_EQ(GsMemory::Address32(0, 10, 8, 0), 64u);
    CHECK_EQ(GsMemory::Address32(0, 10, 0, 8), 128u);
    CHECK_EQ(GsMemory::Address32(0, 10, 64, 0), 2048u);
    CHECK_EQ(GsMemory::Address32(0, 10, 0, 32), 2048u * 10);
    CHECK_EQ(GsMemory::Address32(32, 10, 0, 0), 2048u);
    // Depth swaps block groups: block 0 of PSMZ32 is block 24 of PSMCT32.
    CHECK_EQ(GsMemory::Address32Z(0, 10, 0, 0), 24u * 64);
}

/** Uploads random pixels with an IMAGE transfer, reads them back with a local-to-host one. */
void TestTransferRoundTrip(uint32_t psm) {
    GsMemory          memory;
    RecordingRenderer renderer;
    GsState           gs(memory, &renderer);
    Gif               gif(gs);

    const int            w = 40, h = 24, dx = 13, dy = 7;
    int                  bits = PsmTransferBits(psm);
    size_t               bytes = static_cast<size_t>(w) * h * bits / 8;
    size_t               qwc = (bytes + 15) / 16;
    std::vector<uint8_t> data(qwc * 16);
    std::mt19937         rng(psm);
    for (uint8_t &b : data) {
        b = static_cast<uint8_t>(rng());
    }

    uint32_t        bp = 64, bw = 4;
    std::vector<Qw> packet;
    packet.push_back(MakeQw(GifTag(4, false, false, 0, 0, 1), 0xE));
    packet.push_back(AD(GS_BITBLTBUF, (uint64_t(bp) << 32) | (uint64_t(bw) << 48) | (uint64_t(psm) << 56)));
    packet.push_back(AD(GS_TRXPOS, (uint64_t(dx) << 32) | (uint64_t(dy) << 48)));
    packet.push_back(AD(GS_TRXREG, uint64_t(w) | (uint64_t(h) << 32)));
    packet.push_back(AD(GS_TRXDIR, 0));
    packet.push_back(MakeQw(GifTag(static_cast<uint32_t>(qwc), true, false, 0, 2, 0), 0));
    gif.Write(GIF_PATH3, packet.data(), packet.size());
    gif.Write(GIF_PATH3, data.data(), qwc);

    CHECK(!renderer.written.empty());

    gs.WriteReg(GS_BITBLTBUF, uint64_t(bp) | (uint64_t(bw) << 16) | (uint64_t(psm) << 24));
    gs.WriteReg(GS_TRXPOS, uint64_t(dx) | (uint64_t(dy) << 16));
    gs.WriteReg(GS_TRXREG, uint64_t(w) | (uint64_t(h) << 32));
    gs.WriteReg(GS_TRXDIR, 1);
    std::vector<uint8_t> back(bytes);
    size_t               got = gs.ReadImageData(back.data(), bytes);
    CHECK_EQ(got, bytes);
    bool same = std::memcmp(back.data(), data.data(), bytes) == 0;
    if (!same) {
        std::printf("  transfer round trip differs for psm 0x%02X\n", psm);
    }
    CHECK(same);
}

void TestPsmct24KeepsAlpha() {
    GsMemory memory;
    memory.WritePixel(PSMCT32, 0, 1, 3, 3, 0xAA112233);
    memory.WritePixel(PSMCT24, 0, 1, 3, 3, 0x00445566);
    CHECK_EQ(memory.ReadPixel(PSMCT32, 0, 1, 3, 3), 0xAA445566u);
    memory.WritePixel(PSMT8H, 0, 1, 3, 3, 0x77);
    CHECK_EQ(memory.ReadPixel(PSMCT32, 0, 1, 3, 3), 0x77445566u);
    CHECK_EQ(memory.ReadPixel(PSMT4HH, 0, 1, 3, 3), 0x7u);
    CHECK_EQ(memory.ReadPixel(PSMT4HL, 0, 1, 3, 3), 0x7u);
}

void TestClutAndDecode() {
    GsMemory memory;
    GsState  gs(memory);
    // A 256-colour CLUT in PSMCT32 at block 0x100, entry i = 0x80 << 24 | i * 0x010101.
    const uint32_t cbp = 0x100;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t pos = (i & ~0x18u) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
        memory.WritePixel(PSMCT32, cbp, 1, pos & 15, pos >> 4, 0x80000000u | (i * 0x010101u));
    }
    // An 8x8 PSMT8 texture at block 0x200 holding x + 8y.
    const uint32_t tbp = 0x200;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            memory.WritePixel(PSMT8, tbp, 2, x, y, x + 8 * y);
        }
    }
    uint64_t tex0 = uint64_t(tbp) | (uint64_t(2) << 14) | (uint64_t(PSMT8) << 20) | (uint64_t(3) << 26) |
                    (uint64_t(3) << 30) | (uint64_t(cbp) << 37) | (uint64_t(PSMCT32) << 51) | (uint64_t(1) << 61);
    gs.WriteReg(GS_TEX0_1, tex0);
    std::vector<uint32_t> out(64);
    memory.DecodeTexture(PSMT8, tbp, 2, 8, 8, gs.Clut(), PSMCT32, 0, out.data());
    bool ok = true;
    for (int i = 0; i < 64; i++) {
        ok &= out[i] == (0x80000000u | (i * 0x010101u));
    }
    CHECK(ok);

    // A 16-colour PSMCT16 CLUT at CSA 2.
    GsState gs16(memory);
    for (uint32_t i = 0; i < 16; i++) {
        memory.WritePixel(PSMCT16, 0x300, 1, i & 7, i >> 3, 0x8000 | i);
    }
    uint64_t tex0_4 = uint64_t(0x200) | (uint64_t(2) << 14) | (uint64_t(PSMT4) << 20) | (uint64_t(0x300) << 37) |
                      (uint64_t(PSMCT16) << 51) | (uint64_t(2) << 56) | (uint64_t(1) << 61);
    gs16.WriteReg(GS_TEX0_2, tex0_4);
    CHECK_EQ(gs16.Clut().entries[32 + 5], 0x8005);
}

void TestSpriteAndBatching() {
    GsMemory          memory;
    RecordingRenderer renderer;
    GsState           gs(memory, &renderer);
    Gif               gif(gs);

    // FRAME, XYOFFSET (2048 - 256, 2048 - 224), then two sprites with the same state and a triangle strip.
    std::vector<Qw> p;
    p.push_back(MakeQw(GifTag(3, false, false, 0, 0, 1), 0xE));
    p.push_back(AD(GS_FRAME_1, uint64_t(0) | (uint64_t(8) << 16)));
    p.push_back(AD(GS_XYOFFSET_1, uint64_t(1792 << 4) | (uint64_t(1824 << 4) << 32)));
    p.push_back(AD(GS_ZBUF_1, uint64_t(0x100) | (uint64_t(1) << 24)));
    // PACKED: RGBAQ, XYZ2, XYZ2 for each sprite.
    p.push_back(MakeQw(GifTag(2, false, true, PRIM_SPRITE, 0, 3), 0x551));
    for (int i = 0; i < 2; i++) {
        Qw color = {
            {0x10u * (i + 1), 0x20, 0x30, 0x80}
        };
        p.push_back(color);
        Qw a = {
            {(1792u + 10 + i * 100) << 4, (1824u + 20) << 4, 0x1234, 0}
        };
        Qw b = {
            {(1792u + 50 + i * 100) << 4, (1824u + 60) << 4, 0x1234, 0}
        };
        p.push_back(a);
        p.push_back(b);
    }
    // A Gouraud triangle strip with three vertices through A+D, the last with XYZ2.
    p.push_back(MakeQw(GifTag(6, true, false, 0, 0, 1), 0xE));
    p.push_back(AD(GS_PRIM, PRIM_TRIANGLE_STRIP | (1 << 3)));
    p.push_back(AD(GS_RGBAQ, 0x80FF0000));
    p.push_back(AD(GS_XYZ3, Xyz((1792 + 0) << 4, (1824 + 0) << 4, 5)));
    p.push_back(AD(GS_XYZ3, Xyz((1792 + 32) << 4, (1824 + 0) << 4, 5)));
    p.push_back(AD(GS_RGBAQ, 0x800000FF));
    p.push_back(AD(GS_XYZ2, Xyz((1792 + 0) << 4, (1824 + 32) << 4, 0x2000000)));
    gif.Write(GIF_PATH3, p.data(), p.size());
    gs.Flush();

    // Shading is resolved per vertex, so the sprites and the strip share one batch.
    CHECK_EQ(renderer.batches.size(), static_cast<size_t>(1));
    if (renderer.batches.size() == 1) {
        const auto &v = renderer.batches[0].vertices;
        CHECK_EQ(v.size(), static_cast<size_t>(15));
        CHECK_EQ(renderer.batches[0].state.topology, TOPOLOGY_TRIANGLES);
        CHECK_EQ(v[0].x, 10.0f);
        CHECK_EQ(v[0].y, 20.0f);
        CHECK_EQ(v[4].x, 50.0f);
        CHECK_EQ(v[4].y, 60.0f);
        CHECK_EQ(v[6].x, 110.0f);
        CHECK_EQ(v[6].rgba, 0x80302020u);
        CHECK_EQ(v[12].rgba, 0x80FF0000u);
        CHECK_EQ(v[14].rgba, 0x800000FFu);
        // ZBUF is PSMZ24, so Z is clamped to 24 bits.
        CHECK_EQ(v[14].z, 0xFFFFFFu);
    }
}

/** A stand-in microprogram: copies its input buffer to a GIF packet and kicks it. */
class EchoProgram : public IVu1Program {
public:
    uint32_t runs = 0;
    uint32_t last_top = 0;

    uint32_t Run(Vu1Host &host, uint32_t pc) override {
        Vu1 &vu = host.State();
        runs++;
        last_top = vu.top;
        // The input buffer holds a ready GIF packet; kick it from where it is.
        host.Kick(vu.top);
        return pc;
    }
};

void TestVifAndDma() {
    Machine           m;
    RecordingRenderer renderer;
    m.SetRenderer(&renderer);
    EchoProgram    echo;
    const uint64_t signature[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
    m.vu1.Register(0x10, signature, 2, &echo);

    // A packet in host memory: VIF codes with data, wrapped in a DMA chain.
    std::vector<uint32_t> words;
    auto                  qw = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        words.push_back(a);
        words.push_back(b);
        words.push_back(c);
        words.push_back(d);
    };

    // Tag 0: CNT with MPG of the signature, BASE/OFFSET and STCYCL in the tag's VIF half.
    // Layout: [tag][data...]
    // MPG data: 2 instructions = 1 qw; then two VIF codes padding to a quadword boundary.
    uint32_t mpg_qwc = 2; // MPG code + 4 words + 3 NOPs = 8 words
    qw(0x10000000 | mpg_qwc, 0, 0x03000000 | 0x20 /* BASE 0x20 */, 0x02000000 | 0x40 /* OFFSET 0x40 */);
    words.push_back(0x4A020010); // MPG 2 instructions at 0x10
    words.push_back(static_cast<uint32_t>(signature[0]));
    words.push_back(static_cast<uint32_t>(signature[0] >> 32));
    words.push_back(static_cast<uint32_t>(signature[1]));
    words.push_back(static_cast<uint32_t>(signature[1] >> 32));
    words.push_back(0x01000101); // STCYCL CL=1 WL=1
    words.push_back(0);
    words.push_back(0);

    // Tag 1: CNT whose VIF half starts an UNPACK V4-32 with FLG of 3 quadwords: a GIF packet
    // drawing one sprite. Then MSCAL 0x10 after the data.
    qw(0x10000000 | 4, 0, 0, 0x6C038000 /* UNPACK V4-32, FLG, num 3, addr 0 */);
    Qw tag = MakeQw(GifTag(1, true, true, PRIM_SPRITE, 0, 2), 0x55);
    qw(tag.w[0], tag.w[1], tag.w[2], tag.w[3]);
    qw(0, 0, 0, 0); // XYZ2 (0, 0)
    qw(16 << 4, 16 << 4, 0, 0);
    qw(0x14000010 /* MSCAL 0x10 */, 0, 0, 0);

    // Tag 2: END with a DIRECT of one A+D packet that sets FRAME_1 through PATH2.
    qw(0x70000000 | 2, 0, 0, 0x50000002 /* DIRECT 2 qw */);
    Qw dtag = MakeQw(GifTag(1, true, false, 0, 0, 1), 0xE);
    qw(dtag.w[0], dtag.w[1], dtag.w[2], dtag.w[3]);
    Qw frame = AD(GS_FRAME_1, 0x1234);
    qw(frame.w[0], frame.w[1], frame.w[2], frame.w[3]);

    int tags = m.dma.SendChain(1, words.data(), true);
    m.gs.Flush();
    CHECK_EQ(tags, 3);
    CHECK_EQ(echo.runs, 1u);
    CHECK_EQ(echo.last_top, 0x20u); // first buffer: BASE
    CHECK_EQ(m.gs.Reg(GS_FRAME_1), 0x1234ull);
    CHECK_EQ(renderer.batches.size(), static_cast<size_t>(1));
    if (!renderer.batches.empty()) {
        CHECK_EQ(renderer.batches[0].vertices.size(), static_cast<size_t>(6));
    }

    // A second UNPACK with FLG lands in the other buffer (BASE + OFFSET).
    uint32_t again[] = {0x6C018000, 1, 2, 3, 4, 0x17000000 /* MSCNT */};
    m.vif1.Write(again, 6);
    CHECK_EQ(m.vu1.State().mem[0x60][0], 1u);
    CHECK_EQ(echo.runs, 2u);
    CHECK_EQ(echo.last_top, 0x60u);
}

void TestUnpackModes() {
    Machine m;
    // STCYCL CL=4 WL=2 (skipping), UNPACK V2-16 signed of 4 vectors at 0x100.
    uint32_t a[] = {0x01000204, 0x65040100, 0xFFFF0001, 0x00030002, 0x00050004, 0x00070006};
    m.vif1.Write(a, 6);
    Vu1 &vu = m.vu1.State();
    CHECK_EQ(vu.mem[0x100][0], 1u);
    CHECK_EQ(vu.mem[0x100][1], 0xFFFFFFFFu);
    CHECK_EQ(vu.mem[0x101][0], 2u);
    CHECK_EQ(vu.mem[0x104][0], 4u);
    CHECK_EQ(vu.mem[0x105][1], 7u);

    // STROW + STMOD offset + masked V4-8 unsigned: field W from COL.
    uint32_t b[] = {0x01000101, 0x30000000, 10, 20, 30, 40, 0x31000000, 100, 101, 102, 103,
                    0x05000001, 0x20000000, 0x80 /* W from COL */, 0x7E014200, 0x04030201};
    m.vif1.Write(b, sizeof(b) / 4);
    CHECK_EQ(vu.mem[0x200][0], 11u);
    CHECK_EQ(vu.mem[0x200][2], 33u);
    CHECK_EQ(vu.mem[0x200][3], 100u);

    // V4-5: 0x8000 | B=31 G=0 R=1.
    uint32_t c[] = {0x05000000, 0x6F010300, 0x00007C01};
    m.vif1.Write(c, 3);
    CHECK_EQ(vu.mem[0x300][0], 8u);
    CHECK_EQ(vu.mem[0x300][2], 0xF8u);
    CHECK_EQ(vu.mem[0x300][3], 0u);
}

/** Tag addresses as offsets into one buffer, so the test does not depend on where the heap is. */
class OffsetMap : public MemoryMap {
public:
    explicit OffsetMap(const uint8_t *base) : base_(base) {}

    const uint8_t *Translate(uint32_t addr, bool) override { return base_ + addr; }

private:
    const uint8_t *base_;
};

void TestDmaCallRet() {
    Machine m;
    // Main chain at 0: CALL 0x40, then END. The subroutine at 0x40 sends a DIRECT and RETs.
    alignas(16) uint32_t mem[32] = {
        0x50000000, 0x40, 0, 0, // CALL 0x40, no data
        0x70000000, 0, 0, 0,    // END
    };
    uint32_t *sub = &mem[16];
    sub[0] = 0x60000002; // RET, 2 qw
    sub[3] = 0x50000002; // DIRECT 2
    Qw t = MakeQw(GifTag(1, true, false, 0, 0, 1), 0xE);
    std::memcpy(&sub[4], t.w, 16);
    Qw ad = AD(GS_TEXA, 0x77);
    std::memcpy(&sub[8], ad.w, 16);

    OffsetMap map(reinterpret_cast<const uint8_t *>(mem));
    m.dma.SetMemoryMap(map);
    int tags = m.dma.SendChain(1, mem, true);
    CHECK_EQ(tags, 3);
    CHECK_EQ(m.gs.Reg(GS_TEXA), 0x77ull);
    m.dma.SetMemoryMap(m.memory_map);
}

} // namespace

int main() {
    TestAddressing();
    const uint32_t formats[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4,
                                PSMT8H, PSMT4HL, PSMT4HH, PSMZ32, PSMZ24, PSMZ16};
    for (uint32_t psm : formats) {
        TestTransferRoundTrip(psm);
    }
    TestPsmct24KeepsAlpha();
    TestClutAndDecode();
    TestSpriteAndBatching();
    TestVifAndDma();
    TestUnpackModes();
    TestDmaCallRet();

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("gfx_core_test: all checks passed\n");
    return 0;
}
