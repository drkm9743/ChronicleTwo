// C++ reference for the Dark Cloud 2 (PAL) VU1 microprogram Vu_prog_3dsp
// (3D sprites, billboards drawn as GS SPRITE or rotated quads), written from
// Vu_prog_3dsp.vsm in this folder.
//
// Status: bit-exact against the original microcode run in vu1_interp.cpp
// (test_3dsp.cpp: 200 random batches, 3.071 sprites in both modes, 0 differing
// words; mutations of the guard test, UVs and corners are all detected).
//
// Input at vi12 (BASE/OFFSET buffer), written by mg_sprite.cpp:
//   +0  GIF tag (y,z,w copied to the output tag)
//   +1  x = sprite count n, y = mode (0: axis-aligned, else rotated)
//   +2  n sprites, 5 qw each:
//       pos (x,y,z,w), size (sx, sy, sin, cos), colour (float RGBA),
//       uv0 (float, top-left), uv1 (float, bottom-right)
// Constants: 0x15/0x16 guard band (from VuProgMainSetup), 0x19.x / 0x1A.y size
// scales, 0x3B fog. model_screen comes from VuProgMainSetup (vf10-vf13).
// Output: one GIF packet at vi13 per batch, sent with XGKICK.
//   axis-aligned: 5 qw per sprite  RGBAQ, UV, XYZF2, UV, XYZF2 (two corners)
//   rotated:      9 qw per sprite  RGBAQ, then 4 x (UV, XYZF2) in strip order

#include "vu_progs.h"

namespace {

struct Guard { Vec4 max, min; };

// vf31/vf30. The two DIVs are issued back to back; the second waits for the
// first, so the two ADDQs read different results: the Q range becomes
// (1/max.w, 1/min.w), i.e. a test on w itself.
Guard LoadGuard(const Vec4* m)
{
    Guard g{ m[0x15], m[0x16] };
    const float qmin = 1.0f / g.max.w, qmax = 1.0f / g.min.w;
    g.min.w = qmin;
    g.max.w = qmax;
    return g;
}

Vec4 Project(const Vec4* M, const Vec4& p)
{
    Vec4 c = { M[0].x * p.x + M[1].x * p.y + M[2].x * p.z + M[3].x * p.w,
               M[0].y * p.x + M[1].y * p.y + M[2].y * p.z + M[3].y * p.w,
               M[0].z * p.x + M[1].z * p.y + M[2].z * p.z + M[3].z * p.w,
               M[0].w * p.x + M[1].w * p.y + M[2].w * p.z + M[3].w * p.w };
    const float q = 1.0f / c.w;
    return { c.x * q, c.y * q, c.z * q, q };
}

// Fog for XYZF2, same formula as the water: clamp(off + scale*Q) to [far, near].
float Fog(const Vec4& fog, float q)
{
    return std::max(std::min(fog.x + fog.w * q, fog.y), fog.z);
}

// MAC sign bits x,y,w of (min - lo) and (hi - max): every corner strictly inside.
bool Inside(const Guard& g, const Vec4& lo, const Vec4& hi)
{
    return lo.x > g.min.x && lo.y > g.min.y && lo.w > g.min.w
        && hi.x < g.max.x && hi.y < g.max.y && hi.w < g.max.w;
}

Vec4 ToI4(Vec4 v)
{
    Vec4 r;
    word(r, 0) = ftoi4(v.x); word(r, 1) = ftoi4(v.y);
    word(r, 2) = ftoi4(v.z); word(r, 3) = ftoi4(v.w);
    return r;
}

Vec4 ToI0(Vec4 v)
{
    Vec4 r;
    word(r, 0) = ftoi0(v.x); word(r, 1) = ftoi0(v.y);
    word(r, 2) = ftoi0(v.z); word(r, 3) = ftoi0(v.w);
    return r;
}

} // namespace

void VuProg3DSprite(Vu1State& vu, void (*xgkick)(const Vec4* packet))
{
    Vec4* m = vu.mem;
    vu.out_buffer = (vu.out_buffer == 0x1A4) ? 0x258 : 0x1A4; // __INIT_DATA
    const uint16_t in = vu.top;
    const uint32_t n = word(m[in + 1], 0);
    const bool rotated = word(m[in + 1], 1) != 0;
    const Vec4* spr = &m[in + 2];

    Vec4* out = &m[vu.out_buffer];
    out[0] = m[in];
    word(out[0], 0) = 0x8000u | n;          // EOP | NLOOP n (vi11 is not masked)
    Vec4* o = out + 1;

    const Guard g = LoadGuard(m);
    const Vec4 fog = m[0x3B];
    const float kx = m[0x19].x, ky = m[0x1A].y;
    const Vec4* M = vu.model_screen;

    // The microcode runs n iterations of a do-while, so n == 0 behaves as 65536
    // (vi09 wraps); mg_sprite.cpp never sends an empty batch.
    for (uint32_t i = 0; i < n; ++i, spr += 5) {
        const Vec4 &pos = spr[0], &size = spr[1];
        const Vec4 rgba = ToI0(spr[2]), uv0 = ToI4(spr[3]), uv1 = ToI4(spr[4]);
        const Vec4 c = Project(M, pos);
        const float f = Fog(fog, c.w);

        if (!rotated) {
            // __MAIN_PROG: GS SPRITE, two opposite corners.
            const float hx = size.x * kx * 0.5f * c.w;
            const float hy = size.y * ky * 0.5f * c.w;
            Vec4 lo = { c.x - hx, c.y - hy, c.z, c.w };
            Vec4 hi = { c.x + hx, c.y + hy, c.z, c.w };
            const float adc = Inside(g, lo, hi) ? 0.0f : 2048.0f;
            lo.w = f + adc;
            hi.w = f + adc;
            o[0] = rgba; o[1] = uv0; o[2] = ToI4(lo); o[3] = uv1; o[4] = ToI4(hi);
            o += 5;
        } else {
            // __MAIN_PROG_ROT: size = (sx, sy, sin, cos). Two half-axis vectors
            // a = (cos, sin)*kx*sx/2 and b = (-sin, cos)*ky*sy/2, scaled by Q.
            const float hx = size.x * 0.5f, hy = size.y * 0.5f;
            const float ax = size.w * kx * hx * c.w, ay = size.z * kx * hx * c.w;
            const float bx = -size.z * ky * hy * c.w, by = size.w * ky * hy * c.w;
            const float sx = ax + bx, sy = ay + by;   // vf15 = a + b
            const float dx = bx - ax, dy = by - ay;   // vf16 = b - a
            Vec4 p0 = { c.x - sx, c.y - sy, c.z, c.w }; // vf27
            Vec4 p1 = { c.x - dx, c.y - dy, c.z, c.w }; // vf17
            Vec4 p2 = { c.x + dx, c.y + dy, c.z, c.w }; // vf18
            Vec4 p3 = { c.x + sx, c.y + sy, c.z, c.w }; // vf28
            Vec4 lo = { std::min(std::min(std::min(p0.x, p3.x), p1.x), p2.x),
                        std::min(std::min(std::min(p0.y, p3.y), p1.y), p2.y), 0, c.w };
            Vec4 hi = { std::max(std::max(std::max(p0.x, p3.x), p1.x), p2.x),
                        std::max(std::max(std::max(p0.y, p3.y), p1.y), p2.y), 0, c.w };
            const float adc = Inside(g, lo, hi) ? 0.0f : 2048.0f;
            // ADC +2048 is added to p0 and p1 by the instruction pairs at 0x121
            // (same pair as the branch) and 0x122 (its delay slot), so it applies
            // whatever the test says. Harmless: in a strip the first two vertices
            // close no triangle.
            p0.w = p1.w = f + 2048.0f;
            p2.w = p3.w = f + adc;

            // UVs: corner 1 takes (u1, v0), corner 2 (u0, v1); z,w words from uv0.
            Vec4 uv10 = uv0, uv01 = uv0;
            word(uv10, 0) = word(uv1, 0);
            word(uv01, 1) = word(uv1, 1);
            o[0] = rgba;
            o[1] = uv0;  o[2] = ToI4(p0);
            o[3] = uv10; o[4] = ToI4(p1);
            o[5] = uv01; o[6] = ToI4(p2);
            o[7] = uv1;  o[8] = ToI4(p3);
            o += 9;
        }
    }
    xgkick(out);
}
