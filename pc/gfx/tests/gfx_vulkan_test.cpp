// Tests for the Vulkan renderer, without a window: GS register writes are
// drawn on the GPU and read back through a GS local-to-host transfer, the way
// the game itself would see them. Needs a Vulkan 1.3 driver (lavapipe works);
// without one the test is skipped.

#include "gfx.h"
#include "vulkan/vk_context.h"
#include "vulkan/vk_renderer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

/** Each channel within `tolerance` of the expected value. */
#define CHECK_RGBA(got, want, tolerance)                                                         \
    do {                                                                                         \
        uint32_t g_ = (got), w_ = (want);                                                        \
        bool     ok_ = true;                                                                     \
        for (int c_ = 0; c_ < 32; c_ += 8) {                                                     \
            int d_ = int((g_ >> c_) & 0xFF) - int((w_ >> c_) & 0xFF);                            \
            ok_ = ok_ && d_ <= (tolerance) && d_ >= -(tolerance);                                \
        }                                                                                        \
        if (!ok_) {                                                                              \
            std::printf("FAIL %s:%d: %s = %08x, want %08x\n", __FILE__, __LINE__, #got, g_, w_); \
            failures++;                                                                          \
        }                                                                                        \
    } while (0)

uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }

uint64_t Xyz(int x, int y, uint32_t z) { return uint64_t(x * 16) | (uint64_t(y * 16) << 16) | (uint64_t(z) << 32); }

const uint64_t kTestZAlways = 1 | (1 << 1) | (uint64_t(1) << 16) | (uint64_t(1) << 17); // ATE on, ATST ALWAYS, ZTE, ZTST ALWAYS

struct Fixture {
    VulkanContext   ctx;
    GsMemory        memory;
    GsState         gs{memory};
    VulkanRenderer *renderer = nullptr;

    bool Init() {
        VulkanContextOptions options;
        options.validation = std::getenv("GFX_VALIDATION") != nullptr;
        std::string error;
        if (!ctx.Init(options, &error)) {
            std::printf("gfx_vulkan_test: skipped, no usable Vulkan device (%s)\n", error.c_str());
            return false;
        }
        std::printf("gfx_vulkan_test: device %s\n", ctx.properties.deviceName);
        renderer = new VulkanRenderer(ctx, memory);
        if (!renderer->Init(&error)) {
            std::printf("FAIL renderer init: %s\n", error.c_str());
            failures++;
            return false;
        }
        gs.SetRenderer(renderer);
        return true;
    }

    ~Fixture() {
        delete renderer;
        ctx.Shutdown();
    }

    /** A 64-pixel-wide 32-bit frame at `fbp` (in 2048-word units), no depth, full scissor. */
    void Frame(uint32_t fbp, uint32_t fbmsk = 0) {
        gs.WriteReg(GS_FRAME_1, fbp | (uint64_t(1) << 16) | (uint64_t(PSMCT32) << 24) | (uint64_t(fbmsk) << 32));
        gs.WriteReg(GS_ZBUF_1, uint64_t(1) << 32); // ZMSK
        gs.WriteReg(GS_SCISSOR_1, uint64_t(63) << 16 | uint64_t(63) << 48);
        gs.WriteReg(GS_XYOFFSET_1, 0);
        gs.WriteReg(GS_TEST_1, kTestZAlways);
    }

    void Sprite(uint64_t prim, int x0, int y0, int x1, int y1, uint32_t rgba, uint32_t z = 0) {
        gs.WriteReg(GS_PRIM, prim);
        gs.WriteReg(GS_RGBAQ, rgba | (uint64_t(0x3F800000) << 32));
        gs.WriteReg(GS_XYZ2, Xyz(x0, y0, z));
        gs.WriteReg(GS_XYZ2, Xyz(x1, y1, z));
    }

    /** Reads a rectangle of a 64-wide buffer back with a local-to-host transfer. */
    std::vector<uint32_t> Read(uint32_t bp, uint32_t psm, int x, int y, int w, int h) {
        gs.Flush();
        gs.WriteReg(GS_BITBLTBUF, uint64_t(bp) | (uint64_t(1) << 16) | (uint64_t(psm) << 24));
        gs.WriteReg(GS_TRXPOS, uint64_t(x) | (uint64_t(y) << 16));
        gs.WriteReg(GS_TRXREG, uint64_t(w) | (uint64_t(h) << 32));
        gs.WriteReg(GS_TRXDIR, 1);
        std::vector<uint32_t> out(size_t(w) * h);
        gs.ReadImageData(reinterpret_cast<uint8_t *>(out.data()), out.size() * 4);
        return out;
    }

    uint32_t Pixel(uint32_t bp, int x, int y) { return Read(bp, PSMCT32, x, y, 1, 1)[0]; }

    /** Writes 32-bit pixels with a host-to-local transfer. */
    void Write(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h, const uint32_t *data) {
        gs.Flush();
        gs.WriteReg(GS_BITBLTBUF, (uint64_t(bp) << 32) | (uint64_t(bw) << 48) | (uint64_t(psm) << 56));
        gs.WriteReg(GS_TRXPOS, (uint64_t(x) << 32) | (uint64_t(y) << 48));
        gs.WriteReg(GS_TRXREG, uint64_t(w) | (uint64_t(h) << 32));
        gs.WriteReg(GS_TRXDIR, 0);
        gs.WriteImageData(reinterpret_cast<const uint8_t *>(data), size_t(w) * h * 4);
    }
};

const uint64_t kSprite = 6;
const uint64_t kTriangle = 3;
const uint64_t kIip = 1 << 3, kTme = 1 << 4, kAbe = 1 << 6, kFst = 1 << 8;

void TestFill(Fixture &f) {
    f.Frame(0);
    f.Sprite(kSprite, 0, 0, 64, 64, Rgba(255, 0, 0, 0x80));
    f.Sprite(kSprite, 8, 8, 16, 12, Rgba(0, 255, 0, 0x40));
    CHECK_RGBA(f.Pixel(0, 0, 0), Rgba(255, 0, 0, 0x80), 0);
    CHECK_RGBA(f.Pixel(0, 63, 63), Rgba(255, 0, 0, 0x80), 0);
    // Sprite edges follow GS fill rules: x1 and y1 are not drawn.
    CHECK_RGBA(f.Pixel(0, 8, 8), Rgba(0, 255, 0, 0x40), 0);
    CHECK_RGBA(f.Pixel(0, 15, 11), Rgba(0, 255, 0, 0x40), 0);
    CHECK_RGBA(f.Pixel(0, 16, 11), Rgba(255, 0, 0, 0x80), 0);
    CHECK_RGBA(f.Pixel(0, 15, 12), Rgba(255, 0, 0, 0x80), 0);
    CHECK_RGBA(f.Pixel(0, 7, 8), Rgba(255, 0, 0, 0x80), 0);
}

void TestScissorAndMask(Fixture &f) {
    f.Frame(0);
    f.Sprite(kSprite, 0, 0, 64, 64, Rgba(0, 0, 0, 0));
    f.gs.WriteReg(GS_SCISSOR_1, 4 | (uint64_t(7) << 16) | (uint64_t(4) << 32) | (uint64_t(7) << 48));
    f.Sprite(kSprite, 0, 0, 64, 64, Rgba(10, 20, 30, 40));
    CHECK_RGBA(f.Pixel(0, 4, 4), Rgba(10, 20, 30, 40), 0);
    CHECK_RGBA(f.Pixel(0, 7, 7), Rgba(10, 20, 30, 40), 0);
    CHECK_RGBA(f.Pixel(0, 8, 7), 0, 0);
    CHECK_RGBA(f.Pixel(0, 3, 4), 0, 0);
    // FBMSK keeps the green channel.
    f.Frame(0, 0x0000FF00);
    f.Sprite(kSprite, 0, 0, 64, 64, Rgba(1, 2, 3, 4));
    CHECK_RGBA(f.Pixel(0, 5, 5), Rgba(1, 20, 3, 4), 0);
}

void TestGouraud(Fixture &f) {
    f.Frame(0);
    f.Sprite(kSprite, 0, 0, 64, 64, 0);
    f.gs.WriteReg(GS_PRIM, kTriangle | kIip);
    f.gs.WriteReg(GS_RGBAQ, Rgba(0, 0, 0, 0x80));
    f.gs.WriteReg(GS_XYZ2, Xyz(0, 0, 0));
    f.gs.WriteReg(GS_RGBAQ, Rgba(255, 0, 0, 0x80));
    f.gs.WriteReg(GS_XYZ2, Xyz(64, 0, 0));
    f.gs.WriteReg(GS_RGBAQ, Rgba(0, 0, 0, 0x80));
    f.gs.WriteReg(GS_XYZ2, Xyz(0, 64, 0));
    uint32_t mid = f.Pixel(0, 32, 0);
    uint32_t red = mid & 0xFF;
    CHECK(red > 110 && red < 145);
    CHECK_RGBA(f.Pixel(0, 40, 40), 0, 0); // outside the triangle
}

void TestBlend(Fixture &f) {
    f.Frame(0);
    f.Sprite(kSprite, 0, 0, 64, 64, Rgba(200, 0, 0, 0x80));
    // (Cs - Cd) * As + Cd with As = 0x40 (one half).
    f.gs.WriteReg(GS_ALPHA_1, 0x44);
    f.Sprite(kSprite | kAbe, 0, 0, 64, 64, Rgba(0, 0, 100, 0x40));
    CHECK_RGBA(f.Pixel(0, 10, 10), Rgba(100, 0, 50, 0x40), 1);
    // Cs * FIX + Cd, the additive form.
    f.gs.WriteReg(GS_ALPHA_1, 0x68 | (uint64_t(0x80) << 32));
    f.Sprite(kSprite | kAbe, 0, 0, 64, 64, Rgba(10, 10, 10, 0));
    CHECK_RGBA(f.Pixel(0, 10, 10), Rgba(110, 10, 60, 0), 1);
}

/**
 * The shadow pass of the game: Vu_prog_sdw draws the front faces of each
 * shadow volume with Cs * FIX + Cd (ALPHA 0x68) and the back faces with
 * Cd - Cs * FIX (0x62); what stays non-zero is in shadow.
 */
void TestShadowCounting(Fixture &f) {
    f.Frame(0);
    f.Sprite(kSprite, 0, 0, 64, 64, 0);
    uint64_t fix = uint64_t(0x80) << 32;
    f.gs.WriteReg(GS_ALPHA_1, 0x68 | fix);
    f.Sprite(kSprite | kAbe, 0, 0, 32, 64, Rgba(16, 16, 16, 0));  // volume A, front
    f.Sprite(kSprite | kAbe, 16, 0, 48, 64, Rgba(16, 16, 16, 0)); // volume B, front
    f.gs.WriteReg(GS_ALPHA_1, 0x62 | fix);
    f.Sprite(kSprite | kAbe, 0, 0, 24, 64, Rgba(16, 16, 16, 0)); // A's back face covers less
    CHECK_RGBA(f.Pixel(0, 10, 5) & 0xFFFFFF, 0, 0);              // in and out of A
    CHECK_RGBA(f.Pixel(0, 28, 5) & 0xFFFFFF, Rgba(32, 32, 32, 0), 0);
    CHECK_RGBA(f.Pixel(0, 40, 5) & 0xFFFFFF, Rgba(16, 16, 16, 0), 0);
    CHECK_RGBA(f.Pixel(0, 56, 5) & 0xFFFFFF, 0, 0);
    // Subtraction stops at zero, as with COLCLAMP.
    f.Sprite(kSprite | kAbe, 56, 0, 64, 64, Rgba(16, 16, 16, 0));
    CHECK_RGBA(f.Pixel(0, 60, 5) & 0xFFFFFF, 0, 0);
}

void TestTexture(Fixture &f) {
    // A 4x4 32-bit texture at block 0x800, each texel distinct.
    uint32_t texels[16];
    for (int i = 0; i < 16; i++) {
        texels[i] = Rgba(i * 16, 255 - i * 16, i, 0x80);
    }
    f.Write(0x800, 1, PSMCT32, 0, 0, 4, 4, texels);
    f.Frame(0);
    f.Sprite(kSprite, 0, 0, 64, 64, 0);
    uint64_t tex0 = 0x800 | (uint64_t(1) << 14) | (uint64_t(PSMCT32) << 20) | (uint64_t(2) << 26) |
                    (uint64_t(2) << 30) | (uint64_t(1) << 34) | (uint64_t(1) << 35); // TCC, DECAL
    f.gs.WriteReg(GS_TEX0_1, tex0);
    f.gs.WriteReg(GS_TEX1_1, 0);
    f.gs.WriteReg(GS_CLAMP_1, 0);
    f.gs.WriteReg(GS_PRIM, kSprite | kTme | kFst);
    f.gs.WriteReg(GS_RGBAQ, Rgba(128, 128, 128, 0x80));
    f.gs.WriteReg(GS_UV, 0);
    f.gs.WriteReg(GS_XYZ2, Xyz(0, 0, 0));
    f.gs.WriteReg(GS_UV, (4 * 16) | (uint64_t(4 * 16) << 16));
    f.gs.WriteReg(GS_XYZ2, Xyz(8, 8, 0));
    std::vector<uint32_t> got = f.Read(0, PSMCT32, 0, 0, 8, 8);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            CHECK_RGBA(got[y * 8 + x], texels[(y / 2) * 4 + x / 2], 0);
        }
    }
    // MODULATE: texel * vertex colour / 128.
    f.gs.WriteReg(GS_TEX0_1, tex0 & ~(uint64_t(3) << 35));
    f.gs.WriteReg(GS_RGBAQ, Rgba(64, 64, 64, 0x80));
    f.gs.WriteReg(GS_UV, 0);
    f.gs.WriteReg(GS_XYZ2, Xyz(0, 0, 0));
    f.gs.WriteReg(GS_UV, (4 * 16) | (uint64_t(4 * 16) << 16));
    f.gs.WriteReg(GS_XYZ2, Xyz(8, 8, 0));
    uint32_t t = texels[5];
    CHECK_RGBA(f.Pixel(0, 2, 2), Rgba((t & 0xFF) / 2, ((t >> 8) & 0xFF) / 2, ((t >> 16) & 0xFF) / 2, 0x80), 1);

    // Rewriting the texture memory is seen by the next draw.
    uint32_t white[16];
    for (uint32_t &w : white) {
        w = Rgba(255, 255, 255, 0x80);
    }
    f.Write(0x800, 1, PSMCT32, 0, 0, 4, 4, white);
    f.gs.WriteReg(GS_TEX0_1, tex0);
    f.gs.WriteReg(GS_UV, 0);
    f.gs.WriteReg(GS_XYZ2, Xyz(0, 0, 0));
    f.gs.WriteReg(GS_UV, (4 * 16) | (uint64_t(4 * 16) << 16));
    f.gs.WriteReg(GS_XYZ2, Xyz(8, 8, 0));
    CHECK_RGBA(f.Pixel(0, 3, 3), Rgba(255, 255, 255, 0x80), 0);
}

void TestRenderToTexture(Fixture &f) {
    // Draw into a buffer at page 16, then sample it as a texture into page 0.
    f.Frame(16);
    f.Sprite(kSprite, 0, 0, 64, 64, Rgba(0, 0, 0, 0x80));
    f.Sprite(kSprite, 0, 0, 2, 2, Rgba(9, 99, 199, 0x80));
    f.Frame(0);
    f.Sprite(kSprite, 0, 0, 64, 64, 0);
    uint64_t tex0 = (16 * 32) | (uint64_t(1) << 14) | (uint64_t(PSMCT32) << 20) | (uint64_t(6) << 26) |
                    (uint64_t(6) << 30) | (uint64_t(1) << 34) | (uint64_t(1) << 35);
    f.gs.WriteReg(GS_TEX0_1, tex0);
    f.gs.WriteReg(GS_PRIM, kSprite | kTme | kFst);
    f.gs.WriteReg(GS_UV, 0);
    f.gs.WriteReg(GS_XYZ2, Xyz(10, 10, 0));
    f.gs.WriteReg(GS_UV, (4 * 16) | (uint64_t(4 * 16) << 16));
    f.gs.WriteReg(GS_XYZ2, Xyz(14, 14, 0));
    CHECK_RGBA(f.Pixel(0, 11, 11), Rgba(9, 99, 199, 0x80), 0);
    CHECK_RGBA(f.Pixel(0, 13, 13), Rgba(0, 0, 0, 0x80), 0);
    CHECK(f.renderer->GetStats().target_readbacks > 0);
}

void TestDepth(Fixture &f) {
    f.Frame(0);
    f.Sprite(kSprite, 0, 0, 64, 64, 0);
    // Z buffer at page 64, 24-bit; clear it to 0 then draw near (high Z) before far.
    f.gs.WriteReg(GS_ZBUF_1, 64 | (uint64_t(1) << 24)); // PSMZ24
    f.gs.WriteReg(GS_TEST_1, kTestZAlways);
    f.Sprite(kSprite, 0, 0, 64, 64, 0, 0);
    f.gs.WriteReg(GS_TEST_1, 1 | (1 << 1) | (uint64_t(1) << 16) | (uint64_t(2) << 17)); // Z GEQUAL
    f.Sprite(kSprite, 0, 0, 32, 64, Rgba(0, 255, 0, 0x80), 0x800000);
    f.Sprite(kSprite, 0, 0, 64, 64, Rgba(255, 0, 0, 0x80), 0x400000);
    CHECK_RGBA(f.Pixel(0, 10, 10), Rgba(0, 255, 0, 0x80), 0);
    CHECK_RGBA(f.Pixel(0, 50, 10), Rgba(255, 0, 0, 0x80), 0);
    // The Z buffer reads back with the values drawn.
    // A 24-bit transfer packs three bytes per pixel.
    std::vector<uint32_t> words = f.Read(64 * 32, PSMZ24, 0, 0, 64, 1);
    const uint8_t        *bytes = reinterpret_cast<const uint8_t *>(words.data());
    auto                  z = [&](int x) { return uint32_t(bytes[x * 3]) | (bytes[x * 3 + 1] << 8) | (bytes[x * 3 + 2] << 16); };
    CHECK(z(10) == 0x800000);
    CHECK(z(50) == 0x400000);
}

void TestAlphaTest(Fixture &f) {
    f.Frame(0);
    f.Sprite(kSprite, 0, 0, 64, 64, Rgba(0, 0, 0, 0));
    // ATST GEQUAL 0x40; failing pixels keep RGB only (AFAIL RGB_ONLY).
    uint64_t test = 1 | (uint64_t(5) << 1) | (uint64_t(0x40) << 4) | (uint64_t(3) << 12) | (uint64_t(1) << 16) |
                    (uint64_t(1) << 17);
    f.gs.WriteReg(GS_TEST_1, test);
    f.Sprite(kSprite, 0, 0, 8, 8, Rgba(50, 60, 70, 0x50));
    f.Sprite(kSprite, 8, 0, 16, 8, Rgba(50, 60, 70, 0x10));
    CHECK_RGBA(f.Pixel(0, 2, 2), Rgba(50, 60, 70, 0x50), 0);
    CHECK_RGBA(f.Pixel(0, 10, 2), Rgba(50, 60, 70, 0), 0);
    // AFAIL KEEP drops failing pixels.
    f.gs.WriteReg(GS_TEST_1, test & ~(uint64_t(3) << 12));
    f.Sprite(kSprite, 16, 0, 24, 8, Rgba(50, 60, 70, 0x10));
    CHECK_RGBA(f.Pixel(0, 18, 2), 0, 0);
}

void TestPresentHeadless(Fixture &f) {
    f.gs.WritePrivileged(GS_PRIV_PMODE, 2);
    f.gs.WritePrivileged(GS_PRIV_DISPFB2, 1 << 9);
    f.gs.WritePrivileged(GS_PRIV_DISPLAY2, (uint64_t(3) << 23) | (uint64_t(255) << 32) | (uint64_t(63) << 44));
    uint64_t before = f.renderer->GetStats().frames;
    for (int i = 0; i < 3; i++) {
        f.Frame(0);
        f.Sprite(kSprite, 0, 0, 64, 64, Rgba(i * 50, 0, 0, 0x80));
        f.gs.VSync();
    }
    CHECK(f.renderer->GetStats().frames == before + 3);
    CHECK_RGBA(f.Pixel(0, 1, 1), Rgba(100, 0, 0, 0x80), 0);
}

} // namespace

int main() {
    Fixture f;
    if (!f.Init()) {
        return failures ? 1 : 0;
    }
    TestFill(f);
    TestScissorAndMask(f);
    TestGouraud(f);
    TestBlend(f);
    TestShadowCounting(f);
    TestTexture(f);
    TestRenderToTexture(f);
    TestDepth(f);
    TestAlphaTest(f);
    TestPresentHeadless(f);
    f.renderer->SyncAll();
    const VulkanRenderer::Stats &s = f.renderer->GetStats();
    std::printf("gfx_vulkan_test: %llu draws, %llu pipelines, %llu readbacks, %llu texture uploads\n",
                (unsigned long long) s.draws, (unsigned long long) s.pipelines,
                (unsigned long long) s.target_readbacks, (unsigned long long) s.texture_uploads);
    if (failures) {
        std::printf("gfx_vulkan_test: %d failures\n", failures);
        return 1;
    }
    std::printf("gfx_vulkan_test: all checks passed\n");
    return 0;
}
