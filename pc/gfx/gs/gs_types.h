// Register numbers, pixel formats and field accessors of the PS2 Graphics
// Synthesizer, as the GS User's Manual lays them out. Plain integers only: the
// emulation keeps registers as the 64-bit words the game writes and reads the
// fields it needs through these helpers.

#pragma once

#include <cstdint>

namespace gfx {

/** General-purpose GS registers, by the address A+D and REGLIST packets use. */
enum GsReg : uint8_t {
    GS_PRIM = 0x00,
    GS_RGBAQ = 0x01,
    GS_ST = 0x02,
    GS_UV = 0x03,
    GS_XYZF2 = 0x04,
    GS_XYZ2 = 0x05,
    GS_TEX0_1 = 0x06,
    GS_TEX0_2 = 0x07,
    GS_CLAMP_1 = 0x08,
    GS_CLAMP_2 = 0x09,
    GS_FOG = 0x0A,
    GS_XYZF3 = 0x0C,
    GS_XYZ3 = 0x0D,
    GS_TEX1_1 = 0x14,
    GS_TEX1_2 = 0x15,
    GS_TEX2_1 = 0x16,
    GS_TEX2_2 = 0x17,
    GS_XYOFFSET_1 = 0x18,
    GS_XYOFFSET_2 = 0x19,
    GS_PRMODECONT = 0x1A,
    GS_PRMODE = 0x1B,
    GS_TEXCLUT = 0x1C,
    GS_SCANMSK = 0x22,
    GS_MIPTBP1_1 = 0x34,
    GS_MIPTBP1_2 = 0x35,
    GS_MIPTBP2_1 = 0x36,
    GS_MIPTBP2_2 = 0x37,
    GS_TEXA = 0x3B,
    GS_FOGCOL = 0x3D,
    GS_TEXFLUSH = 0x3F,
    GS_SCISSOR_1 = 0x40,
    GS_SCISSOR_2 = 0x41,
    GS_ALPHA_1 = 0x42,
    GS_ALPHA_2 = 0x43,
    GS_DIMX = 0x44,
    GS_DTHE = 0x45,
    GS_COLCLAMP = 0x46,
    GS_TEST_1 = 0x47,
    GS_TEST_2 = 0x48,
    GS_PABE = 0x49,
    GS_FBA_1 = 0x4A,
    GS_FBA_2 = 0x4B,
    GS_FRAME_1 = 0x4C,
    GS_FRAME_2 = 0x4D,
    GS_ZBUF_1 = 0x4E,
    GS_ZBUF_2 = 0x4F,
    GS_BITBLTBUF = 0x50,
    GS_TRXPOS = 0x51,
    GS_TRXREG = 0x52,
    GS_TRXDIR = 0x53,
    GS_HWREG = 0x54,
    GS_SIGNAL = 0x60,
    GS_FINISH = 0x61,
    GS_LABEL = 0x62,
    GS_REG_COUNT = 0x63,
};

/** Privileged registers, by their offset from 0x12000000. */
enum GsPrivReg : uint32_t {
    GS_PRIV_PMODE = 0x0000,
    GS_PRIV_SMODE1 = 0x0010,
    GS_PRIV_SMODE2 = 0x0020,
    GS_PRIV_SRFSH = 0x0030,
    GS_PRIV_SYNCH1 = 0x0040,
    GS_PRIV_SYNCH2 = 0x0050,
    GS_PRIV_SYNCV = 0x0060,
    GS_PRIV_DISPFB1 = 0x0070,
    GS_PRIV_DISPLAY1 = 0x0080,
    GS_PRIV_DISPFB2 = 0x0090,
    GS_PRIV_DISPLAY2 = 0x00A0,
    GS_PRIV_EXTBUF = 0x00B0,
    GS_PRIV_EXTDATA = 0x00C0,
    GS_PRIV_EXTWRITE = 0x00D0,
    GS_PRIV_BGCOLOR = 0x00E0,
    GS_PRIV_CSR = 0x1000,
    GS_PRIV_IMR = 0x1010,
    GS_PRIV_BUSDIR = 0x1040,
    GS_PRIV_SIGLBLID = 0x1080,
};

/** Pixel storage formats (PSM). The Z formats share the colour layouts with other block tables. */
enum GsPsm : uint8_t {
    PSMCT32 = 0x00,
    PSMCT24 = 0x01,
    PSMCT16 = 0x02,
    PSMCT16S = 0x0A,
    PSMT8 = 0x13,
    PSMT4 = 0x14,
    PSMT8H = 0x1B,
    PSMT4HL = 0x24,
    PSMT4HH = 0x2C,
    PSMZ32 = 0x30,
    PSMZ24 = 0x31,
    PSMZ16 = 0x32,
    PSMZ16S = 0x3A,
};

/** Primitive types of PRIM. */
enum GsPrimType : uint8_t {
    PRIM_POINT = 0,
    PRIM_LINE = 1,
    PRIM_LINE_STRIP = 2,
    PRIM_TRIANGLE = 3,
    PRIM_TRIANGLE_STRIP = 4,
    PRIM_TRIANGLE_FAN = 5,
    PRIM_SPRITE = 6,
    PRIM_INVALID = 7,
};

/** Size in bytes of the memory the GS addresses. */
constexpr uint32_t kGsMemoryBytes = 4 * 1024 * 1024;
/** A page is 8 KiB, 32 blocks of 256 bytes. */
constexpr uint32_t kGsPageBytes = 8192;
constexpr uint32_t kGsPageCount = kGsMemoryBytes / kGsPageBytes;
constexpr uint32_t kGsBlockBytes = 256;

/** Extracts `bits` bits of `v` starting at bit `lo`. */
constexpr uint32_t Bits(uint64_t v, int lo, int bits) {
    return static_cast<uint32_t>((v >> lo) & ((uint64_t(1) << bits) - 1));
}

constexpr bool IsZFormat(uint32_t psm) { return (psm & 0x30) == 0x30; }

/** Bits per pixel of a storage format. */
constexpr int PsmBits(uint32_t psm) {
    switch (psm) {
        case PSMCT32:
        case PSMCT24:
        case PSMT8H:
        case PSMT4HL:
        case PSMT4HH:
        case PSMZ32:
        case PSMZ24:
            return 32;
        case PSMCT16:
        case PSMCT16S:
        case PSMZ16:
        case PSMZ16S:
            return 16;
        case PSMT8:
            return 8;
        case PSMT4:
            return 4;
        default:
            return 32;
    }
}

/** Bits a transfer carries per pixel (PSMCT24 and PSMZ24 travel packed as 3 bytes). */
constexpr int PsmTransferBits(uint32_t psm) {
    switch (psm) {
        case PSMCT24:
        case PSMZ24:
            return 24;
        case PSMT8H:
            return 8;
        case PSMT4HL:
        case PSMT4HH:
            return 4;
        default:
            return PsmBits(psm);
    }
}

/** Page dimensions in pixels. */
constexpr int PsmPageWidth(uint32_t psm) {
    return PsmBits(psm) == 32 ? 64 : PsmBits(psm) == 16 ? 64
                                                        : 128;
}

constexpr int PsmPageHeight(uint32_t psm) {
    return PsmBits(psm) == 32 ? 32 : PsmBits(psm) == 16 ? 64
                                 : PsmBits(psm) == 8    ? 64
                                                        : 128;
}

/** Formats whose pixels a render target holds as colour (32, 24 or 16 bits). */
constexpr bool IsColorFormat(uint32_t psm) {
    return psm == PSMCT32 || psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S;
}

/** Formats a render target can hold, colour or depth. */
constexpr bool IsTargetFormat(uint32_t psm) {
    return IsColorFormat(psm) || psm == PSMZ32 || psm == PSMZ24 || psm == PSMZ16 || psm == PSMZ16S;
}

/** Largest Z a depth format stores; vertices beyond it are clamped. */
constexpr uint32_t ZFormatMax(uint32_t zpsm) {
    switch (zpsm & 0xF) {
        case 0: // PSMZ32
            return 0xFFFFFFFFu;
        case 1: // PSMZ24
            return 0x00FFFFFFu;
        default: // PSMZ16, PSMZ16S
            return 0x0000FFFFu;
    }
}

} // namespace gfx
