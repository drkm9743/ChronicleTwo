#include "gs_memory.h"

#include <algorithm>
#include <cstring>

namespace gfx {

namespace {

// Block order inside a page. A page holds 32 blocks; these give the block
// number at each block position, row by row.

const uint8_t kBlockTable32[4][8] = {
    {0,  1,  4,  5,  16, 17, 20, 21},
    {2,  3,  6,  7,  18, 19, 22, 23},
    {8,  9,  12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31},
};

const uint8_t kBlockTable16[8][4] = {
    {0,  2,  8,  10},
    {1,  3,  9,  11},
    {4,  6,  12, 14},
    {5,  7,  13, 15},
    {16, 18, 24, 26},
    {17, 19, 25, 27},
    {20, 22, 28, 30},
    {21, 23, 29, 31},
};

const uint8_t kBlockTable16S[8][4] = {
    {0,  2,  16, 18},
    {1,  3,  17, 19},
    {8,  10, 24, 26},
    {9,  11, 25, 27},
    {4,  6,  20, 22},
    {5,  7,  21, 23},
    {12, 14, 28, 30},
    {13, 15, 29, 31},
};

const uint8_t kBlockTable8[4][8] = {
    {0,  1,  4,  5,  16, 17, 20, 21},
    {2,  3,  6,  7,  18, 19, 22, 23},
    {8,  9,  12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31},
};

const uint8_t kBlockTable4[8][4] = {
    {0,  2,  8,  10},
    {1,  3,  9,  11},
    {4,  6,  12, 14},
    {5,  7,  13, 15},
    {16, 18, 24, 26},
    {17, 19, 25, 27},
    {20, 22, 28, 30},
    {21, 23, 29, 31},
};

// Pixel order inside a block (four columns of two rows for 32-bit formats),
// in units of the format's pixel size.

const uint8_t kColumnTable32[8][8] = {
    {0,  1,  4,  5,  8,  9,  12, 13},
    {2,  3,  6,  7,  10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29},
    {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45},
    {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61},
    {50, 51, 54, 55, 58, 59, 62, 63},
};

const uint8_t kColumnTable16[8][16] = {
    {0,   2,   8,   10,  16,  18,  24,  26,  1,   3,   9,   11,  17,  19,  25,  27 },
    {4,   6,   12,  14,  20,  22,  28,  30,  5,   7,   13,  15,  21,  23,  29,  31 },
    {32,  34,  40,  42,  48,  50,  56,  58,  33,  35,  41,  43,  49,  51,  57,  59 },
    {36,  38,  44,  46,  52,  54,  60,  62,  37,  39,  45,  47,  53,  55,  61,  63 },
    {64,  66,  72,  74,  80,  82,  88,  90,  65,  67,  73,  75,  81,  83,  89,  91 },
    {68,  70,  76,  78,  84,  86,  92,  94,  69,  71,  77,  79,  85,  87,  93,  95 },
    {96,  98,  104, 106, 112, 114, 120, 122, 97,  99,  105, 107, 113, 115, 121, 123},
    {100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127},
};

const uint8_t kColumnTable8[16][16] = {
    {0,   4,   16,  20,  32,  36,  48,  52,  2,   6,   18,  22,  34,  38,  50,  54 },
    {8,   12,  24,  28,  40,  44,  56,  60,  10,  14,  26,  30,  42,  46,  58,  62 },
    {33,  37,  49,  53,  1,   5,   17,  21,  35,  39,  51,  55,  3,   7,   19,  23 },
    {41,  45,  57,  61,  9,   13,  25,  29,  43,  47,  59,  63,  11,  15,  27,  31 },
    {96,  100, 112, 116, 64,  68,  80,  84,  98,  102, 114, 118, 66,  70,  82,  86 },
    {104, 108, 120, 124, 72,  76,  88,  92,  106, 110, 122, 126, 74,  78,  90,  94 },
    {65,  69,  81,  85,  97,  101, 113, 117, 67,  71,  83,  87,  99,  103, 115, 119},
    {73,  77,  89,  93,  105, 109, 121, 125, 75,  79,  91,  95,  107, 111, 123, 127},
    {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182},
    {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
    {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151},
    {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
    {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214},
    {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
    {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247},
    {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255},
};

const uint16_t kColumnTable4[16][32] = {
    {0,   8,   32,  40,  64,  72,  96,  104, 2,   10,  34,  42,  66,  74,  98,  106,
     4,   12,  36,  44,  68,  76,  100, 108, 6,   14,  38,  46,  70,  78,  102, 110},
    {16,  24,  48,  56,  80,  88,  112, 120, 18,  26,  50,  58,  82,  90,  114, 122,
     20,  28,  52,  60,  84,  92,  116, 124, 22,  30,  54,  62,  86,  94,  118, 126},
    {65,  73,  97,  105, 1,   9,   33,  41,  67,  75,  99,  107, 3,   11,  35,  43,
     69,  77,  101, 109, 5,   13,  37,  45,  71,  79,  103, 111, 7,   15,  39,  47 },
    {81,  89,  113, 121, 17,  25,  49,  57,  83,  91,  115, 123, 19,  27,  51,  59,
     85,  93,  117, 125, 21,  29,  53,  61,  87,  95,  119, 127, 23,  31,  55,  63 },
    {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170,
     196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
    {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186,
     212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
    {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235,
     133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
    {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251,
     149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255},
    {256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362,
     260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366},
    {272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378,
     276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382},
    {321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299,
     325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303},
    {337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315,
     341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319},
    {448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426,
     452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430},
    {464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442,
     468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446},
    {385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491,
     389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495},
    {401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507,
     405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511},
};

// The depth formats use the colour block tables with block numbers 24-31 and
// 0-7 swapped with 8-15 and 16-23: XOR 0x18.
const uint32_t kZBlockXor = 0x18;

uint32_t BlockNumber32(uint32_t bp, uint32_t bw, int x, int y) {
    return bp + (y & ~0x1F) * bw + ((x >> 1) & ~0x1F) + kBlockTable32[(y >> 3) & 3][(x >> 3) & 7];
}

uint32_t BlockNumber16(const uint8_t (&table)[8][4], uint32_t bp, uint32_t bw, int x, int y) {
    return bp + ((y >> 1) & ~0x1F) * bw + ((x >> 1) & ~0x1F) + table[(y >> 3) & 7][(x >> 4) & 3];
}

uint16_t Pack16(uint32_t rgba) {
    uint32_t r = (rgba >> 3) & 0x1F;
    uint32_t g = (rgba >> 11) & 0x1F;
    uint32_t b = (rgba >> 19) & 0x1F;
    uint32_t a = (rgba >> 31) & 1;
    return static_cast<uint16_t>(r | (g << 5) | (b << 10) | (a << 15));
}

uint32_t Expand16(uint32_t c) {
    uint32_t r = (c & 0x1F) << 3;
    uint32_t g = ((c >> 5) & 0x1F) << 3;
    uint32_t b = ((c >> 10) & 0x1F) << 3;
    uint32_t a = (c & 0x8000) ? 0x80 : 0;
    return r | (g << 8) | (b << 16) | (a << 24);
}

/** Position of CLUT entry `i` inside the CLUT texture the game stores (CSM1). */
void ClutPosition(uint32_t i, uint32_t count, int &x, int &y) {
    if (count == 16) {
        x = static_cast<int>(i & 7);
        y = static_cast<int>(i >> 3);
        return;
    }
    // 256-entry CLUTs are stored as 16x16 with bits 3 and 4 of the index swapped.
    uint32_t pos = (i & ~0x18u) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
    x = static_cast<int>(pos & 15);
    y = static_cast<int>(pos >> 4);
}

} // namespace

GsMemory::GsMemory() : data_(new uint8_t[kGsMemoryBytes]()) {}

uint32_t GsMemory::Address32(uint32_t bp, uint32_t bw, int x, int y) {
    return ((BlockNumber32(bp, bw, x, y) << 6) + kColumnTable32[y & 7][x & 7]) & 0xFFFFF;
}

uint32_t GsMemory::Address32Z(uint32_t bp, uint32_t bw, int x, int y) {
    return (((BlockNumber32(bp, bw, x, y) ^ kZBlockXor) << 6) + kColumnTable32[y & 7][x & 7]) & 0xFFFFF;
}

uint32_t GsMemory::Address16(uint32_t bp, uint32_t bw, int x, int y) {
    return ((BlockNumber16(kBlockTable16, bp, bw, x, y) << 7) + kColumnTable16[y & 7][x & 15]) & 0x1FFFFF;
}

uint32_t GsMemory::Address16S(uint32_t bp, uint32_t bw, int x, int y) {
    return ((BlockNumber16(kBlockTable16S, bp, bw, x, y) << 7) + kColumnTable16[y & 7][x & 15]) & 0x1FFFFF;
}

uint32_t GsMemory::Address16Z(uint32_t bp, uint32_t bw, int x, int y) {
    return (((BlockNumber16(kBlockTable16, bp, bw, x, y) ^ kZBlockXor) << 7) + kColumnTable16[y & 7][x & 15]) &
           0x1FFFFF;
}

uint32_t GsMemory::Address16SZ(uint32_t bp, uint32_t bw, int x, int y) {
    return (((BlockNumber16(kBlockTable16S, bp, bw, x, y) ^ kZBlockXor) << 7) + kColumnTable16[y & 7][x & 15]) &
           0x1FFFFF;
}

uint32_t GsMemory::Address8(uint32_t bp, uint32_t bw, int x, int y) {
    uint32_t block =
        bp + ((y >> 1) & ~0x1F) * (bw >> 1) + ((x >> 2) & ~0x1F) + kBlockTable8[(y >> 4) & 3][(x >> 4) & 7];
    return ((block << 8) + kColumnTable8[y & 15][x & 15]) & 0x3FFFFF;
}

uint32_t GsMemory::Address4(uint32_t bp, uint32_t bw, int x, int y) {
    uint32_t block =
        bp + ((y >> 2) & ~0x1F) * (bw >> 1) + ((x >> 2) & ~0x1F) + kBlockTable4[(y >> 4) & 7][(x >> 5) & 3];
    return ((block << 9) + kColumnTable4[y & 15][x & 31]) & 0x7FFFFF;
}

uint32_t GsMemory::ReadPixel(uint32_t psm, uint32_t bp, uint32_t bw, int x, int y) const {
    const uint8_t *m = data_.get();
    uint32_t       word;
    uint16_t       half;
    switch (psm) {
        case PSMCT32:
            std::memcpy(&word, m + Address32(bp, bw, x, y) * 4, 4);
            return word;
        case PSMCT24:
            std::memcpy(&word, m + Address32(bp, bw, x, y) * 4, 4);
            return word & 0xFFFFFF;
        case PSMZ32:
            std::memcpy(&word, m + Address32Z(bp, bw, x, y) * 4, 4);
            return word;
        case PSMZ24:
            std::memcpy(&word, m + Address32Z(bp, bw, x, y) * 4, 4);
            return word & 0xFFFFFF;
        case PSMCT16:
            std::memcpy(&half, m + Address16(bp, bw, x, y) * 2, 2);
            return half;
        case PSMCT16S:
            std::memcpy(&half, m + Address16S(bp, bw, x, y) * 2, 2);
            return half;
        case PSMZ16:
            std::memcpy(&half, m + Address16Z(bp, bw, x, y) * 2, 2);
            return half;
        case PSMZ16S:
            std::memcpy(&half, m + Address16SZ(bp, bw, x, y) * 2, 2);
            return half;
        case PSMT8:
            return m[Address8(bp, bw, x, y)];
        case PSMT4: {
            uint32_t a = Address4(bp, bw, x, y);
            return (m[a >> 1] >> ((a & 1) * 4)) & 0xF;
        }
        case PSMT8H:
            std::memcpy(&word, m + Address32(bp, bw, x, y) * 4, 4);
            return word >> 24;
        case PSMT4HL:
            std::memcpy(&word, m + Address32(bp, bw, x, y) * 4, 4);
            return (word >> 24) & 0xF;
        case PSMT4HH:
            std::memcpy(&word, m + Address32(bp, bw, x, y) * 4, 4);
            return word >> 28;
        default:
            return 0;
    }
}

void GsMemory::WritePixel(uint32_t psm, uint32_t bp, uint32_t bw, int x, int y, uint32_t value) {
    uint8_t *m = data_.get();
    uint32_t word;
    uint16_t half = static_cast<uint16_t>(value);
    auto     merge32 = [&](uint32_t addr, uint32_t mask) {
        uint8_t *p = m + addr * 4;
        std::memcpy(&word, p, 4);
        word = (word & ~mask) | (value & mask);
        std::memcpy(p, &word, 4);
    };
    switch (psm) {
        case PSMCT32:
            std::memcpy(m + Address32(bp, bw, x, y) * 4, &value, 4);
            break;
        case PSMCT24:
            merge32(Address32(bp, bw, x, y), 0x00FFFFFF);
            break;
        case PSMZ32:
            std::memcpy(m + Address32Z(bp, bw, x, y) * 4, &value, 4);
            break;
        case PSMZ24:
            merge32(Address32Z(bp, bw, x, y), 0x00FFFFFF);
            break;
        case PSMCT16:
            std::memcpy(m + Address16(bp, bw, x, y) * 2, &half, 2);
            break;
        case PSMCT16S:
            std::memcpy(m + Address16S(bp, bw, x, y) * 2, &half, 2);
            break;
        case PSMZ16:
            std::memcpy(m + Address16Z(bp, bw, x, y) * 2, &half, 2);
            break;
        case PSMZ16S:
            std::memcpy(m + Address16SZ(bp, bw, x, y) * 2, &half, 2);
            break;
        case PSMT8:
            m[Address8(bp, bw, x, y)] = static_cast<uint8_t>(value);
            break;
        case PSMT4: {
            uint32_t a = Address4(bp, bw, x, y);
            uint8_t &b = m[a >> 1];
            int      shift = (a & 1) * 4;
            b = static_cast<uint8_t>((b & ~(0xF << shift)) | ((value & 0xF) << shift));
            break;
        }
        case PSMT8H:
            value <<= 24;
            merge32(Address32(bp, bw, x, y), 0xFF000000);
            break;
        case PSMT4HL:
            value <<= 24;
            merge32(Address32(bp, bw, x, y), 0x0F000000);
            break;
        case PSMT4HH:
            value <<= 28;
            merge32(Address32(bp, bw, x, y), 0xF0000000);
            break;
        default:
            break;
    }
}

uint32_t GsMemory::ReadColor32(uint32_t psm, uint32_t bp, uint32_t bw, int x, int y) const {
    uint32_t v = ReadPixel(psm, bp, bw, x, y);
    if (PsmBits(psm) == 16) {
        return Expand16(v);
    }
    return v;
}

void GsMemory::WriteColor32(uint32_t psm, uint32_t bp, uint32_t bw, int x, int y, uint32_t rgba) {
    if (PsmBits(psm) == 16) {
        WritePixel(psm, bp, bw, x, y, Pack16(rgba));
    } else {
        WritePixel(psm, bp, bw, x, y, rgba);
    }
}

void GsMemory::LoadClut(GsClut &clut, uint32_t cbp, uint32_t cpsm, uint32_t csm, uint32_t csa, uint32_t count,
                        uint32_t texclut) const {
    bool is32 = cpsm == PSMCT32 || cpsm == PSMCT24;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t color;
        if (csm == 0) {
            int x, y;
            ClutPosition(i, count, x, y);
            color = ReadPixel(is32 ? uint32_t(PSMCT32) : cpsm, cbp, 1, x, y);
        } else {
            // CSM2: a row of 16-bit entries at (COU * 16, COV) in a buffer CBW wide.
            uint32_t cbw = Bits(texclut, 0, 6);
            int      x = static_cast<int>(Bits(texclut, 6, 6) * 16 + i);
            int      y = static_cast<int>(Bits(texclut, 12, 10));
            color = ReadPixel(PSMCT16, cbp, cbw, x, y);
            is32 = false;
        }
        if (is32) {
            uint32_t slot = ((csa & 15) * 16 + i) & 255;
            clut.entries[slot] = static_cast<uint16_t>(color);
            clut.entries[slot + 256] = static_cast<uint16_t>(color >> 16);
        } else {
            clut.entries[(csa * 16 + i) & 511] = static_cast<uint16_t>(color);
        }
    }
}

void GsMemory::DecodeTexture(uint32_t psm, uint32_t bp, uint32_t bw, int width, int height, const GsClut &clut,
                             uint32_t cpsm, uint32_t csa, uint32_t *out) const {
    bool     indexed = psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
    bool     clut32 = cpsm == PSMCT32 || cpsm == PSMCT24;
    bool     four = psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH;
    uint32_t offset = four ? csa * 16 : 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t v;
            if (indexed) {
                uint32_t index = ReadPixel(psm, bp, bw, x, y);
                if (clut32) {
                    uint32_t slot = (offset + index) & 255;
                    v = clut.entries[slot] | (static_cast<uint32_t>(clut.entries[slot + 256]) << 16);
                } else {
                    v = Expand16(clut.entries[(offset + index) & 511]);
                }
            } else {
                v = ReadColor32(psm, bp, bw, x, y);
                if (psm == PSMCT24 || psm == PSMZ24) {
                    v &= 0x00FFFFFF;
                }
            }
            out[y * width + x] = v;
        }
    }
}

void GsMemory::MarkWritten(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) {
    uint64_t stamp = NextStamp();
    ForEachPage(bp, bw, psm, rect, [&](uint32_t page) { page_stamp_[page] = stamp; });
}

uint64_t GsMemory::RegionStamp(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) const {
    uint64_t stamp = 0;
    ForEachPage(bp, bw, psm, rect, [&](uint32_t page) { stamp = std::max(stamp, page_stamp_[page]); });
    return stamp;
}

} // namespace gfx
