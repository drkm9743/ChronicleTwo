// C++ reference for two Dark Cloud 2 (PAL) VU1 microprograms, written from
// the disassembly in Vu_progmain.vsm and Vu_prog_wtr.vsm (same folder).
//
// Status: bit-exact against the original microcode run in vu1_interp.cpp
// (test_wtr.cpp: 200 random batches, 4.236 vertices, 0 differing words).
// Pipeline effects (Q latency, MAC flag timing) were resolved by reading the
// stall rules; each place that depends on them says so.
//
// VU1 data memory is 1024 quadwords. Addresses below are quadword indices.

#include "vu_progs.h"

#include <cfloat>

// ---------------------------------------------------------------------------
// Vu_progmain, entry 0 (VIF MSCAL 0): per-object setup shared by every overlay.
// Runs __main and stops (the E bit sits on the delay slot of `jr vi15`).
// Entry 2 (MSCAL 2) just jumps to 0x064, the overlay's __INIT_DATA.
// ---------------------------------------------------------------------------
void VuProgMainSetup(Vu1State& vu)
{
    Vec4* m = vu.mem;
    const Vec4* world = &m[0x08];   // model -> world rows (vf15-vf18)
    const Vec4* light = &m[0x0C];   // vf19-vf21: x, y, z components of 4 light directions (one light per column)

    for (int i = 0; i < 4; ++i) vu.model_screen[i] = m[0x04 + i]; // vf10-vf13
    for (int i = 0; i < 5; ++i) vu.light_color[i] = m[0x0F + i];  // vf05-vf09

    // vf02-vf04: light directions transformed by the model's 3x3 rotation:
    // light_in_model[k] = sum_j light[j] * world[k][j]  (columns x,y,z of each row)
    Vec4 lm[3];
    for (int k = 0; k < 3; ++k) {
        const Vec4& r = world[k];
        lm[k] = { light[0].x * r.x + light[1].x * r.y + light[2].x * r.z,
                  light[0].y * r.x + light[1].y * r.y + light[2].y * r.z,
                  light[0].z * r.x + light[1].z * r.y + light[2].z * r.z,
                  light[0].w * r.x + light[1].w * r.y + light[2].w * r.z };
    }
    // vf31 = per-component squared length across the three rows; each column of
    // lm is then normalised: lm[*].c *= 1/sqrt(vf31.c). This removes the model's
    // scale from the light directions.
    float len2[4] = {0, 0, 0, 0};
    for (int k = 0; k < 3; ++k) {
        len2[0] += lm[k].x * lm[k].x; len2[1] += lm[k].y * lm[k].y;
        len2[2] += lm[k].z * lm[k].z; len2[3] += lm[k].w * lm[k].w;
    }
    // Before normalising, qw 0x2C = lm[0]*e.x + lm[1]*e.y + lm[2]*e.z
    // (e = qw 0x18). What the overlays use it for is still to be confirmed.
    const Vec4 e = m[0x18];
    m[0x2C] = { lm[0].x * e.x + lm[1].x * e.y + lm[2].x * e.z,
                lm[0].y * e.x + lm[1].y * e.y + lm[2].y * e.z,
                lm[0].z * e.x + lm[1].z * e.y + lm[2].z * e.z,
                lm[0].w * e.x + lm[1].w * e.y + lm[2].w * e.z };
    // RSQRT gives Q = 1/sqrt(len2) and MULQ multiplies by it. A light column
    // that is all zero (w always, and unused lights: scenes with one light)
    // gives 1/sqrt(0): the VU returns FLT_MAX instead of inf, so 0 * Q stays 0
    // where IEEE would give NaN.
    float rs[4];
    for (int c = 0; c < 4; ++c) {
        rs[c] = 1.0f / std::sqrt(len2[c]);
        if (std::isinf(rs[c])) rs[c] = FLT_MAX;
    }
    for (int k = 0; k < 3; ++k) {
        lm[k].x *= rs[0]; lm[k].y *= rs[1]; lm[k].z *= rs[2]; lm[k].w *= rs[3];
    }
    for (int k = 0; k < 3; ++k) vu.light_model[k] = lm[k]; // kept in vf02-vf04

    // Mirror test: triple product of the model rows (vf24.x). If negative the
    // model is mirrored and bit 0x20 is stored at qw 0x1E.x, which flips culling.
    const Vec4 &a = world[0], &b = world[1], &c = world[2];
    float det = (a.y * b.z - a.z * b.y) * c.x + (a.z * b.x - a.x * b.z) * c.y
              + (a.x * b.y - a.y * b.x) * c.z;
    word(m[0x1E], 0) = det < 0.0f ? 0x20 : 0x00;

    // Guard band: copy the global limits (qw 0x39/0x3A, written by
    // mgFlushRenderInfo) to qw 0x15/0x16. Without the scissor flag (bit 2 of
    // qw 0x26.x) x,y are widened to the full GS range 1..4095.
    Vec4 gmax = m[0x39], gmin = m[0x3A];
    if ((word(m[0x26], 0) & 0x2) == 0) {
        gmax.x = gmax.y = 4095.0f;
        gmin.x = gmin.y = 1.0f;
    }
    m[0x15] = gmax;
    m[0x16] = gmin;

    vu.out_buffer = 0x1A4; // vi13
}

// ---------------------------------------------------------------------------
// Vu_prog_wtr, one batch (MSCAL 2 the first time, MSCNT afterwards).
// Input at vi12 (BASE/OFFSET buffer), written by water.cpp:
//   +0  GIF tag (PRIM, regs RGBAQ, UV, XYZF2)
//   +1  x = vertex count n
//   +2  n positions, then n slope vectors
// Constants: 0x15/0x16 guard band (from VuProgMainSetup), 0x18 screen size,
// 0x19 screen offset, 0x1A colour, 0x1B surface params, 0x3B fog.
// Output: GIF packet at vi13, sent with XGKICK.
// ---------------------------------------------------------------------------

void VuProgWater(Vu1State& vu, void (*xgkick)(const Vec4* packet))
{
    Vec4* m = vu.mem;
    vu.out_buffer = (vu.out_buffer == 0x1A4) ? 0x258 : 0x1A4; // __INIT_DATA
    const uint16_t in = vu.top;
    const int n = static_cast<int>(word(m[in + 1], 0));
    const Vec4* pos = &m[in + 2];
    const Vec4* slope = pos + n;

    Vec4* out = &m[vu.out_buffer];
    out[0] = m[in];                         // GIF tag, then x = EOP | NLOOP n
    word(out[0], 0) = 0x8000u | static_cast<uint32_t>(n);
    Vec4* o = out + 1;

    Vec4 gmax = m[0x15], gmin = m[0x16];
    // Q/W range: min.w = 1/max.w, max.w unchanged (two chained DIVs).
    gmin.w = 1.0f / gmax.w;
    const Vec4 size = m[0x18], offset = m[0x19], colour = m[0x1A], surf = m[0x1B];
    const Vec4 fog = m[0x3B]; // offset, near clamp, far clamp, scale

    uint32_t inside1 = 0, inside2 = 0; // vi08, vi09: previous two vertices
    for (int i = 0; i < n; ++i) {
        Vec4 p = pos[i], s = slope[i], rgba = colour;
        p.y *= surf.x;                      // wave height
        s.x *= surf.y; s.y *= surf.y;       // refraction offset strength
        rgba.w *= s.w;

        const Vec4* M = vu.model_screen;
        Vec4 c = { M[0].x * p.x + M[1].x * p.y + M[2].x * p.z + M[3].x * p.w,
                   M[0].y * p.x + M[1].y * p.y + M[2].y * p.z + M[3].y * p.w,
                   M[0].z * p.x + M[1].z * p.y + M[2].z * p.z + M[3].z * p.w,
                   M[0].w * p.x + M[1].w * p.y + M[2].w * p.z + M[3].w * p.w };
        const float q = 1.0f / c.w;
        c.x *= q; c.y *= q; c.z *= q; c.w = q;

        float f = std::max(std::min(fog.x + fog.w * q, fog.y), fog.z);  // XYZF2 fog
        float fade = std::clamp(-0.25f + 375.0f * q, 0.0f, 1.0f); // near alpha fade

        // Inside test on x, y and Q (MAC sign bits 0xD0 of min-p and p-max).
        uint32_t inside = (c.x > gmin.x && c.x < gmax.x && c.y > gmin.y && c.y < gmax.y
                           && c.w > gmin.w && c.w < gmax.w) ? 0xD0 : 0;
        float adc = ((inside & inside1 & inside2) == 0xD0) ? 0.0f : 2048.0f;
        inside2 = inside1;
        inside1 = inside;

        // UV: screen position relative to the GS offset, plus the slope,
        // clamped to [1, size]. The water samples the frame it is drawn over.
        float u = std::min(std::max(c.x - offset.x + s.x, 1.0f), size.x);
        float v = std::min(std::max(c.y - offset.y + s.y, 1.0f), size.y);
        rgba.w *= fade;

        Vec4 q_rgba, q_uv, q_xyzf;
        word(q_rgba, 0) = ftoi0(rgba.x); word(q_rgba, 1) = ftoi0(rgba.y);
        word(q_rgba, 2) = ftoi0(rgba.z); word(q_rgba, 3) = ftoi0(rgba.w);
        word(q_uv, 0) = ftoi4(u); word(q_uv, 1) = ftoi4(v);
        word(q_uv, 2) = ftoi4(std::max(c.z - offset.z + s.z, 1.0f));
        word(q_uv, 3) = ftoi4(std::max(s.w + c.w - offset.w, 1.0f));
        word(q_xyzf, 0) = ftoi4(c.x); word(q_xyzf, 1) = ftoi4(c.y);
        word(q_xyzf, 2) = ftoi4(c.z); word(q_xyzf, 3) = ftoi4(f + adc);
        *o++ = q_rgba;
        *o++ = q_uv;
        *o++ = q_xyzf;
    }
    xgkick(out);
}
