// Checks the libgraph, libdma and libpkt shims against the renderer: the
// double buffer clears through the GS, images go in and out of GS memory,
// and VIF1 and GIF chains built with libpkt reach the GS registers. Opens a
// window; SDL_VIDEO_DRIVER=offscreen runs it without one.

#include "gfx.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Last: libgraph.h defines R, G, B, A as macros.
#include <libdma.h>
#include <libgraph.h>
#include <libpkt.h>

namespace {

int fails = 0;
#define CHECK(c)                                        \
    do {                                                \
        if (!(c)) {                                     \
            std::printf("FAIL %d: %s\n", __LINE__, #c); \
            fails++;                                    \
        }                                               \
    } while (0)
alignas(16) u_int buf[4096];
alignas(16) u_long128 gbuf[256];
alignas(16) u_int pix[640 * 4];
} // namespace

int main() {
    sceDmaReset(1);
    sceGsResetPath();
    sceGsResetGraph(0, SCE_GS_INTERLACE, SCE_GS_PAL, 0);
    sceGsDBuff db;
    sceGsSetDefDBuff(&db, SCE_GS_PSMCT32, 640, 448, SCE_GS_ZGEQUAL, SCE_GS_PSMZ24, 1);
    db.clear0.rgbaq.R = 10;
    db.clear0.rgbaq.G = 20;
    db.clear0.rgbaq.B = 30;
    db.clear0.rgbaq.A = 0x80;
    sceGsSwapDBuff(&db, 0);
    gfx::Machine &m = gfx::GetMachine();
    CHECK(((m.gs.ReadPrivileged(gfx::GS_PRIV_DISPFB2) & 0x1FF)) == 0);
    // draw0 draws into buffer 1 (FBP 140)
    sceGsStoreImage si;
    sceGsSetDefStoreImage(&si, 140 * 32, 10, SCE_GS_PSMCT32, 0, 0, 640, 4);
    sceGsExecStoreImage(&si, (u_long128 *) pix);
    CHECK(pix[0] == 0x801E140Au);
    CHECK(pix[639 + 640 * 3] == 0x801E140Au);
    // load image round trip
    u_int src[16];
    for (int i = 0; i < 16; i++) {
        src[i] = 0x01020304u * i;
    }
    sceGsLoadImage li;
    sceGsSetDefLoadImage(&li, 0x3000, 1, SCE_GS_PSMCT32, 0, 0, 4, 4);
    sceGsExecLoadImage(&li, (u_long128 *) src);
    sceGsSetDefStoreImage(&si, 0x3000, 1, SCE_GS_PSMCT32, 0, 0, 4, 4);
    u_int back[16] = {};
    sceGsExecStoreImage(&si, (u_long128 *) back);
    CHECK(std::memcmp(src, back, sizeof(src)) == 0);
    // VIF packet: CNT + DIRECT + GIF tag A+D FRAME_1, END
    sceVif1Packet pk;
    sceVif1PkInit(&pk, buf);
    sceVif1PkCnt(&pk, 0);
    sceVif1PkOpenDirectCode(&pk, 0);
    u_long128 tag = (u_long128) ((u_long) 1 << 60 | (u_long) 1 << 15) | ((u_long128) 0xE << 64);
    sceVif1PkOpenGifTag(&pk, tag);
    sceVif1PkAddGsAD(&pk, SCE_GS_FRAME_1, 0x1234);
    sceVif1PkAddGsAD(&pk, SCE_GS_ALPHA_1, 0x44);
    sceVif1PkCloseGifTag(&pk);
    sceVif1PkCloseDirectCode(&pk);
    sceVif1PkEnd(&pk, 0);
    sceVif1PkTerminate(&pk);
    CHECK((buf[0] & 0xFFFF) == 3); // gif tag + 2 A+D
    CHECK((buf[3] >> 24) == 0x50 && (buf[3] & 0xFFFF) == 3);
    CHECK((buf[4] & 0x7FFF) == 2); // NLOOP
    sceDmaChan *ch1 = sceDmaGetChan(1);
    ch1->chcr.TTE = 1;
    sceDmaSend(ch1, pk.pBase);
    CHECK(m.gs.Reg(gfx::GS_FRAME_1) == 0x1234);
    CHECK(m.gs.Reg(gfx::GS_ALPHA_1) == 0x44);
    // GIF packet with CNT and END through channel 2
    sceGifPacket gp;
    sceGifPkInit(&gp, gbuf);
    sceGifPkReset(&gp);
    sceGifPkCnt(&gp, 0, 0, 0);
    sceGifPkOpenGifTag(&gp, tag);
    sceGifPkAddGsAD(&gp, SCE_GS_TEXA, 0x77);
    sceGifPkCloseGifTag(&gp);
    sceGifPkEnd(&gp, 0, 0, 0);
    sceGifPkOpenGifTag(&gp, tag);
    sceGifPkAddGsAD(&gp, SCE_GS_FOGCOL, 0x55);
    sceGifPkCloseGifTag(&gp);
    sceGifPkTerminate(&gp);
    sceDmaSend(sceDmaGetChan(2), gp.pBase);
    CHECK(m.gs.Reg(gfx::GS_TEXA) == 0x77);
    CHECK(m.gs.Reg(gfx::GS_FOGCOL) == 0x55);
    int f0 = sceGsSyncV(0), f1 = sceGsSyncV(0);
    CHECK(f0 != f1);
    std::printf(fails ? "shim_test: %d failures\n" : "shim_test: all checks passed\n", fails);
    return fails != 0;
}
