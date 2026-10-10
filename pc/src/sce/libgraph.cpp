// libgraph on the emulated GS of pc/gfx: the double buffer, the display
// registers, image transfers and the field clock. Packets go through the GIF
// exactly as the SDK would send them, so what the GS sees is the same.

#include "../ee/kernel.h"
#include "../platform/pc_graphics.h"
#include "gfx.h"

#include <cstdint>
#include <cstring>

// Last: libgraph.h defines R, G, B, A, PSM and friends as macros.
#include <libgraph.h>

namespace {

/** The GIF tag of an A+D packet of `nloop` registers. */
void AdTag(void *out, uint64_t nloop) {
    uint64_t tag[2] = {nloop | (uint64_t(1) << 15) | (uint64_t(1) << 60), 0xE};
    std::memcpy(out, tag, sizeof(tag));
}

template <typename T>
void Put(T &field, uint64_t value) {
    static_assert(sizeof(T) == 8, "a GS register is 64 bits");
    std::memcpy(&field, &value, 8);
}

template <typename T>
uint64_t Get(const T &field) {
    static_assert(sizeof(T) == 8, "a GS register is 64 bits");
    uint64_t value;
    std::memcpy(&value, &field, 8);
    return value;
}

/** Pages a w x h buffer of `psm` takes. */
uint32_t BufferPages(uint32_t psm, int w, int h) {
    int pw = gfx::PsmPageWidth(psm), ph = gfx::PsmPageHeight(psm);
    return uint32_t(((w + pw - 1) / pw) * ((h + ph - 1) / ph));
}

struct GraphMode {
    int  interlace = 1;
    int  ffmd = 0;
    bool pal = true;
};

GraphMode graph_mode;

int (*vsync_callback)(int) = nullptr;

void SetDispEnv(sceGsDispEnv &disp, uint32_t psm, int w, int h, uint32_t fbp) {
    int magh = (2560 + w - 1) / w - 1;
    disp.pmode = 0x66; // circuit 2, alpha from PMODE.ALP
    disp.smode2 = uint64_t(graph_mode.interlace & 1) | (uint64_t(graph_mode.ffmd & 1) << 1);
    disp.dispfb = fbp | (uint64_t(w / 64) << 9) | (uint64_t(psm) << 15);
    uint64_t display = uint64_t(graph_mode.pal ? 652 : 636) | (uint64_t(graph_mode.pal ? 72 : 50) << 12) |
                       (uint64_t(magh) << 23) | (uint64_t(w * (magh + 1) - 1) << 32) | (uint64_t(h - 1) << 44);
    Put(disp.display, display);
    disp.bgcolor = 0;
}

void SetDrawEnv(sceGsDrawEnv1 &draw, uint32_t psm, int w, int h, uint32_t fbp, uint32_t zbp, int ztest, int zpsm) {
    uint64_t ofx = uint64_t(2048 - w / 2) << 4, ofy = uint64_t(2048 - h / 2) << 4;
    Put(draw.frame1, fbp | (uint64_t(w / 64) << 16) | (uint64_t(psm) << 24));
    draw.frame1addr = SCE_GS_FRAME_1;
    Put(draw.zbuf1, zbp | (uint64_t(zpsm & 0xF) << 24) | (uint64_t(ztest == 0) << 32));
    draw.zbuf1addr = SCE_GS_ZBUF_1;
    Put(draw.xyoffset1, ofx | (ofy << 32));
    draw.xyoffset1addr = SCE_GS_XYOFFSET_1;
    Put(draw.scissor1, uint64_t(w - 1) << 16 | uint64_t(h - 1) << 48);
    draw.scissor1addr = SCE_GS_SCISSOR_1;
    Put(draw.prmodecont, 1);
    draw.prmodecontaddr = SCE_GS_PRMODECONT;
    Put(draw.colclamp, 1);
    draw.colclampaddr = 0x46; // COLCLAMP
    Put(draw.dthe, 0);
    draw.dtheaddr = 0x45; // DTHE
    uint64_t ztst = ztest == 0 ? SCE_GS_ALWAYS : uint64_t(ztest);
    Put(draw.test1, (uint64_t(1) << 16) | (ztst << 17));
    draw.test1addr = SCE_GS_TEST_1;
}

void SetClear(sceGsClear &clear, const sceGsDrawEnv1 &draw, int w, int h) {
    uint64_t xyoffset = Get(draw.xyoffset1);
    uint64_t ofx = xyoffset & 0xFFFF, ofy = (xyoffset >> 32) & 0xFFFF;
    Put(clear.test0, (uint64_t(1) << 16) | (uint64_t(SCE_GS_ALWAYS) << 17));
    clear.test0addr = SCE_GS_TEST_1;
    Put(clear.prim, SCE_GS_PRIM_SPRITE);
    clear.primaddr = SCE_GS_PRIM;
    Put(clear.rgbaq, uint64_t(0x3F800000) << 32);
    clear.rgbaqaddr = SCE_GS_RGBAQ;
    Put(clear.xyz2_0, ofx | (ofy << 16));
    clear.xyz2_0addr = SCE_GS_XYZ2;
    Put(clear.xyz2_1, (ofx + (uint64_t(w) << 4)) | ((ofy + (uint64_t(h) << 4)) << 16));
    clear.xyz2_1addr = SCE_GS_XYZ2;
    Put(clear.test1, Get(draw.test1));
    clear.test1addr = SCE_GS_TEST_1;
}

/** The transfer packets: GIF tag, BITBLTBUF, TRXPOS, TRXREG, TRXDIR, then the byte count. */
void SetTransfer(u_long (*chain)[2], uint64_t bitbltbuf, uint64_t trxpos, int w, int h, uint32_t psm, int dir) {
    AdTag(chain[0], 4);
    chain[1][0] = bitbltbuf;
    chain[1][1] = SCE_GS_BITBLTBUF;
    chain[2][0] = trxpos;
    chain[2][1] = SCE_GS_TRXPOS;
    chain[3][0] = uint64_t(w) | (uint64_t(h) << 32);
    chain[3][1] = SCE_GS_TRXREG;
    chain[4][0] = uint64_t(dir);
    chain[4][1] = SCE_GS_TRXDIR;
    chain[5][0] = uint64_t(w) * h * gfx::PsmTransferBits(psm) / 8;
    chain[5][1] = 0;
}

} // namespace

extern "C" {

void sceGsResetGraph(short mode, short inter, short omode, short ffmd) {
    graph_mode.interlace = inter;
    graph_mode.ffmd = ffmd;
    graph_mode.pal = omode == SCE_GS_PAL;
    pc::GraphicsStart(graph_mode.pal);
    gfx::Machine &m = gfx::GetMachine();
    if (mode == 0) {
        m.Reset();
    }
    m.gs.WritePrivileged(gfx::GS_PRIV_SMODE2, uint64_t(inter & 1) | (uint64_t(ffmd & 1) << 1));
}

void sceGsResetPath() {
    gfx::Machine &m = gfx::GetMachine();
    m.gif.Reset();
    m.vif1.Reset();
}

int sceGsSyncV(int mode) {
    (void) mode;
    // The EE kernel's vertical blank interrupt paces the fields and runs the
    // callback; this waits for the next one, as the SDK polls CSR.VSINT.
    pc::ee::WaitVBlankAfter(pc::ee::VBlankCount());
    pc::GraphicsPresent();
    return int((gfx::GetMachine().gs.ReadPrivileged(gfx::GS_PRIV_CSR) >> 13) & 1);
}

extern "C" void pcWaitVBlank() {
    pc::ee::WaitVBlankAfter(pc::ee::VBlankCount());
    pc::GraphicsPresent();
}

int sceGsSyncPath(int mode, u_short timeout) {
    // Transfers finish inside sceDmaSend; nothing is ever in flight.
    (void) mode;
    (void) timeout;
    gfx::GetMachine().gs.Flush();
    return 0;
}

void sceGsSyncVCallback(int (*callback)(int)) {
    vsync_callback = callback;
    pc::ee::SetVSyncCallback(callback);
}

void sceGsSetDefDBuff(sceGsDBuff *db, int psm, int w, int h, int ztest, int zpsm, int clear) {
    std::memset(db, 0, sizeof(*db));
    uint32_t fbp0 = 0;
    uint32_t fbp1 = BufferPages(psm, w, h);
    uint32_t zbp = fbp1 * 2;
    SetDispEnv(db->disp[0], psm, w, h, fbp0);
    SetDispEnv(db->disp[1], psm, w, h, fbp1);
    // Each half shows one buffer and draws into the other.
    SetDrawEnv(db->draw0, psm, w, h, fbp1, zbp, ztest, zpsm);
    SetDrawEnv(db->draw1, psm, w, h, fbp0, zbp, ztest, zpsm);
    SetClear(db->clear0, db->draw0, w, h);
    SetClear(db->clear1, db->draw1, w, h);
    uint64_t nloop = clear ? 14 : 8;
    AdTag(&db->giftag0, nloop);
    AdTag(&db->giftag1, nloop);
}

void sceGsSwapDBuff(sceGsDBuff *db, int id) {
    gfx::Machine       &m = gfx::GetMachine();
    const sceGsDispEnv &disp = db->disp[id & 1];
    m.gs.WritePrivileged(gfx::GS_PRIV_PMODE, disp.pmode);
    m.gs.WritePrivileged(gfx::GS_PRIV_SMODE2, disp.smode2);
    m.gs.WritePrivileged(gfx::GS_PRIV_DISPFB2, disp.dispfb);
    m.gs.WritePrivileged(gfx::GS_PRIV_DISPLAY2, Get(disp.display));
    m.gs.WritePrivileged(gfx::GS_PRIV_BGCOLOR, disp.bgcolor);
    // GIF tag, draw environment and clear sit one after another.
    const sceGifTag *tag = (id & 1) ? &db->giftag1 : &db->giftag0;
    uint64_t         nloop;
    std::memcpy(&nloop, tag, 8);
    m.gif.Write(gfx::GIF_PATH3, tag, 1 + (nloop & 0x7FFF));
}

void sceGsSetHalfOffset(sceGsDrawEnv1 *env, short offx, short offy, short field) {
    uint64_t ofx = uint64_t(uint16_t(offx)) << 4;
    uint64_t ofy = (uint64_t(uint16_t(offy)) << 4) + ((field & 1) ? 8 : 0);
    Put(env->xyoffset1, ofx | (ofy << 32));
}

void sceGsSetDefStoreImage(sceGsStoreImage *si, short sbp, short sbw, short spsm, short ssax, short ssay, short rrw,
                           short rrh) {
    std::memset(si, 0, sizeof(*si));
    SetTransfer(si->chain, uint64_t(uint16_t(sbp)) | (uint64_t(sbw) << 16) | (uint64_t(spsm) << 24),
                uint64_t(ssax) | (uint64_t(ssay) << 16), rrw, rrh, uint32_t(spsm), 1);
}

void sceGsExecStoreImage(sceGsStoreImage *si, u_long128 *dest) {
    gfx::Machine &m = gfx::GetMachine();
    m.gif.Write(gfx::GIF_PATH3, si->chain, 5);
    m.gs.ReadImageData(reinterpret_cast<uint8_t *>(dest), size_t(si->chain[5][0]));
}

void sceGsSetDefLoadImage(sceGsLoadImage *li, short dbp, short dbw, short dpsm, short dsax, short dsay, short rrw,
                          short rrh) {
    std::memset(li, 0, sizeof(*li));
    SetTransfer(li->chain, (uint64_t(uint16_t(dbp)) << 32) | (uint64_t(dbw) << 48) | (uint64_t(dpsm) << 56),
                (uint64_t(dsax) << 32) | (uint64_t(dsay) << 48), rrw, rrh, uint32_t(dpsm), 0);
}

void sceGsExecLoadImage(sceGsLoadImage *li, u_long128 *source) {
    gfx::Machine &m = gfx::GetMachine();
    m.gif.Write(gfx::GIF_PATH3, li->chain, 5);
    m.gs.WriteImageData(reinterpret_cast<const uint8_t *>(source), size_t(li->chain[5][0]));
}

} // extern "C"
