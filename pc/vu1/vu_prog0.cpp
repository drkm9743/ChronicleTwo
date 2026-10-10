// C++ reference for the Dark Cloud 2 (PAL) VU1 microprogram Vu_prog0, the one
// that draws models (mgCVisualMDT, mgCVisualMotionMDT). Written from
// Vu_prog0.vsm in this folder.
//
// Status: complete. The skeleton here (flag dispatch, __LIGHTING_OFF,
// __LIGHTING, __LIGHTING_VC + C_MUL / C_ADD, __TRANSPERS and the four
// __OUTPUT*) plus vu_prog0_blend.cpp (skinning, reflection), vu_prog0_light.cpp
// (point and material light) and vu_prog0_clip.cpp (scissor path), all
// bit-exact against the original microcode in vu1_interp.cpp with PCSX2
// rules. test_prog0_all.cpp mixes every flag at random: 20.000 batches, every
// packet identical. Known gap: in 2 of those batches (strips, scissor + no
// light) the clip buffers' scratch memory at 0x363/0x39F ends up different;
// the packets are the same.
//
// Batch at vi12 (mgCVisualMDT::CreateFacePacket):
//   +0 GIF tag (x: EOP | NLOOP n), +1 header {np, nn, nt, type}
//   +2 then the streams, one after the other: np positions, nn normals,
//   nt texture coordinates (S, T, 1), then vertex colours.
// The program works in place: lighting overwrites the normals with colours
// (or writes one constant colour per vertex at 0x320 when the light is off),
// __TRANSPERS overwrites positions and texture coordinates with their
// projected values, and __OUTPUT* builds the GIF packet at vi13.
//
// Flags (qw 0x26.x, see the VU1 doc): 0x01 guard test, 0x02 scissor,
// 0x04 back-face cull, 0x08 material lighting, 0x10 point lights, 0x20 no
// light, 0x40 skinning, 0x80 additive vertex colour, 0x100 reflection.
// Header type: 0x100 vertex colours, 0x200 no normals, (type & 7) == 4 strip.

#include "vu_prog0_internal.h"

namespace prog0 {

// --- lighting -----------------------------------------------------------------

// __LIGHTINGC_MUL / __LIGHTINGC_ADD, chosen by __LIGHTING_VC on flag 0x80.
void VertexColours(Vu1State& vu, Batch& b)
{
    const uint32_t flags = word(vu.mem[0x26], 0);
    for (uint32_t i = 0; i < b.n; ++i) {
        const Vec4 c = b.colour[i], v = b.vcol[i];
        Vec4 r;
        if (flags & 0x80) {
            // Additive: vertex colour * 128 plus the lit colour; alpha becomes
            // vertex alpha * 128 * vertex alpha.
            r = { v.x * 128.0f, v.y * 128.0f, v.z * 128.0f, v.w * 128.0f };
            r.x += c.x; r.y += c.y; r.z += c.z;
            r.w *= v.w;
        } else {
            r = Mul(c, v);
        }
        b.colour[i] = MinY(r, kMaxColour);
    }
}

// __LIGHTING_OFF: one constant colour, object colour * material, at 0x320.
void LightingOff(Vu1State& vu, Batch& b)
{
    const Vec4 c = MinY(Mul(vu.mem[0x14], vu.mem[0x28]), kMaxColour);
    b.colour = &vu.mem[0x320];
    for (uint32_t i = 0; i < b.n; ++i) b.colour[i] = c;
    if (b.type & 0x100) VertexColours(vu, b);
}

// __LIGHTING: ambient + 4 directional lights, all scaled by the material
// diffuse colour (qw 0x28). qw 0x0C-0x0E hold the x, y and z components of the
// 4 light directions (one light per column), so lm[k].c is component k of light c.
void Lighting(Vu1State& vu, Batch& b)
{
    const Vec4 mat = vu.mem[0x28];
    Vec4 L[5];
    for (int k = 0; k < 5; ++k) L[k] = Mul(vu.light_color[k], mat);
    const Vec4* lm = vu.light_model;
    for (uint32_t i = 0; i < b.n; ++i) {
        const Vec4 nr = b.colour[i];
        Vec4 d = { lm[0].x * nr.x + lm[1].x * nr.y + lm[2].x * nr.z,
                   lm[0].y * nr.x + lm[1].y * nr.y + lm[2].y * nr.z,
                   lm[0].z * nr.x + lm[1].z * nr.y + lm[2].z * nr.z,
                   lm[0].w * nr.x + lm[1].w * nr.y + lm[2].w * nr.z };
        d = { std::max(d.x, 0.0f), std::max(d.y, 0.0f), std::max(d.z, 0.0f), std::max(d.w, 0.0f) };
        Vec4 c;
        c.x = L[4].x + L[0].x * d.x; c.x += L[1].x * d.y; c.x += L[2].x * d.z; c.x += L[3].x * d.w;
        c.y = L[4].y + L[0].y * d.x; c.y += L[1].y * d.y; c.y += L[2].y * d.z; c.y += L[3].y * d.w;
        c.z = L[4].z + L[0].z * d.x; c.z += L[1].z * d.y; c.z += L[2].z * d.z; c.z += L[3].z * d.w;
        c.w = L[4].w + L[0].w * d.x; c.w += L[1].w * d.y; c.w += L[2].w * d.z; c.w += L[3].w * d.w;
        b.colour[i] = MinY(c, kMaxColour);
    }
    if (b.type & 0x100) VertexColours(vu, b);
}

// --- projection -----------------------------------------------------------------

// __TRANSPERS: positions become (x/w, y/w, z/w, 1/w) and texture coordinates
// (S/w, T/w, 1/w): the ST register's third word feeds Q for perspective-
// correct texturing. The fourth word of each UV is left as it was (the
// microcode leaves a stale register value there; the GS ignores it).
void TransPers(Vu1State& vu, Batch& b)
{
    const Vec4* M = vu.model_screen;
    for (uint32_t i = 0; i < b.n; ++i) {
        const Vec4 p = b.pos[i];
        const Vec4 c = { M[0].x * p.x + M[1].x * p.y + M[2].x * p.z + M[3].x * p.w,
                         M[0].y * p.x + M[1].y * p.y + M[2].y * p.z + M[3].y * p.w,
                         M[0].z * p.x + M[1].z * p.y + M[2].z * p.z + M[3].z * p.w,
                         M[0].w * p.x + M[1].w * p.y + M[2].w * p.z + M[3].w * p.w };
        const float q = 1.0f / c.w;
        b.pos[i] = { c.x * q, c.y * q, c.z * q, q };
        b.uv[i].x *= q; b.uv[i].y *= q; b.uv[i].z *= q;
    }
}

// --- output ---------------------------------------------------------------------

float Fog(const Vec4& fog, float q)
{
    return std::max(std::min(fog.x + fog.w * q, fog.y), fog.z);
}

// Same as Vu_prog_3dsp: two chained DIVs make the Q range (1/max.w, 1/min.w).
Guard LoadGuard(const Vec4* m)
{
    Guard g{ m[0x15], m[0x16] };
    const float qmin = 1.0f / g.max.w, qmax = 1.0f / g.min.w;
    g.min.w = qmin;
    g.max.w = qmax;
    return g;
}

// MAC sign bits x, y, w of (min - p) and (p - max).
bool Inside(const Guard& g, const Vec4& p)
{
    return p.x > g.min.x && p.y > g.min.y && p.w > g.min.w
        && p.x < g.max.x && p.y < g.max.y && p.w < g.max.w;
}

Vec4 Xyzf(const Vec4& p, float fog) { return ToI4({ p.x, p.y, p.z, fog }); }

// Output tag: EOP bit kept from the input, NLOOP = n, the rest copied.
void TagWithCount(Vec4* out, const Batch& b)
{
    out[0] = *b.tag;
    word(out[0], 0) = (word(*b.tag, 0) & 0x8000u) | (b.n & 0xFFFF);
}

// __OUTPUT: no guard test.
void Output(Vu1State& vu, Batch& b, Vec4* out)
{
    const Vec4 fog = vu.mem[0x3B];
    TagWithCount(out, b);
    Vec4* o = out + 1;
    for (uint32_t i = 0; i < b.n; ++i) {
        *o++ = b.uv[i];
        *o++ = ToI0(b.colour[i]);
        *o++ = Xyzf(b.pos[i], Fog(fog, b.pos[i].w));
    }
}

// __OUTPUTC (flag 0x01): ADC on a vertex unless it and the two before it are
// inside the guard band. The input tag goes out unchanged.
void OutputClip(Vu1State& vu, Batch& b, Vec4* out)
{
    const Vec4 fog = vu.mem[0x3B];
    const Guard g = LoadGuard(vu.mem);
    out[0] = *b.tag;
    Vec4* o = out + 1;
    bool in1 = false, in2 = false; // vi08, vi09 start at 0
    for (uint32_t i = 0; i < b.n; ++i) {
        const bool in = Inside(g, b.pos[i]);
        const float adc = (in && in1 && in2) ? 0.0f : 2048.0f;
        in2 = in1; in1 = in;
        *o++ = b.uv[i];
        *o++ = ToI0(b.colour[i]);
        *o++ = Xyzf(b.pos[i], Fog(fog, b.pos[i].w) + adc);
    }
}

// Screen-space winding for back-face culling: sign of z of a x b.
bool NegativeZ(const Vec4& a, const Vec4& b) { return std::signbit(a.x * b.y - b.x * a.y); }

// __OUTPUTCN (flag 0x04, triangle lists): a triangle is kept when its three
// vertices are inside the guard band and its winding matches the mirror flag
// (qw 0x1E.x, bit 0x20 from Vu_progmain); otherwise its third vertex gets ADC.
void OutputCull(Vu1State& vu, Batch& b, Vec4* out)
{
    const Vec4 fog = vu.mem[0x3B];
    const Guard g = LoadGuard(vu.mem);
    const bool mirrored = word(vu.mem[0x1E], 0) & 0x20;
    TagWithCount(out, b);
    Vec4* o = out + 1;
    // The loop runs while the counter (decremented by 3) is not zero.
    for (uint32_t i = 0; i < b.n; i += 3, o += 9) {
        const Vec4 *p = &b.pos[i];
        const Vec4 e1 = { p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z, 0 };
        const Vec4 e2 = { p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z, 0 };
        const bool keep = Inside(g, p[0]) && Inside(g, p[1]) && Inside(g, p[2])
                       && NegativeZ(e2, e1) == mirrored;
        for (int k = 0; k < 3; ++k) {
            float f = Fog(fog, p[k].w);
            if (k == 2 && !keep) f += 2048.0f;
            o[3 * k + 0] = b.uv[i + k];
            o[3 * k + 1] = ToI0(b.colour[i + k]);
            o[3 * k + 2] = Xyzf(p[k], f);
        }
    }
}

// __OUTPUTCN_STR (flag 0x04, strips): per vertex, the triangle it closes is
// kept when the vertex and the two before it are inside the guard band and
// the winding (flipped every vertex, as strips alternate) matches the mirror
// flag. The decision for the first two vertices depends on registers left by
// earlier batches (vf30, vf31, vi07, vi08) and closes no triangle; this
// version marks them ADC. The input tag goes out unchanged.
void OutputCullStrip(Vu1State& vu, Batch& b, Vec4* out)
{
    const Vec4 fog = vu.mem[0x3B];
    const Guard g = LoadGuard(vu.mem);
    const bool mirrored = word(vu.mem[0x1E], 0) & 0x20;
    out[0] = *b.tag;
    Vec4* o = out + 1;
    float sign = 1.0f;
    bool in1 = false, in2 = false;
    for (uint32_t i = 0; i < b.n; ++i, sign = -sign) {
        const Vec4& p = b.pos[i];
        const bool in = Inside(g, p);
        bool keep = false;
        if (i >= 2) {
            const Vec4 &p1 = b.pos[i - 1], &p2 = b.pos[i - 2];
            const Vec4 a = { p1.x - p.x, p1.y - p.y, p1.z - p.z, p1.w - p.w };
            const Vec4 c = { p2.x - p.x, p2.y - p.y, p2.z - p.z, p2.w - p.w };
            const float z = (a.x * c.y - c.x * a.y) * sign;
            keep = in && in1 && in2 && std::signbit(z) == mirrored;
        }
        in2 = in1; in1 = in;
        *o++ = b.uv[i];
        *o++ = ToI0(b.colour[i]);
        *o++ = Xyzf(p, Fog(fog, p.w) + (keep ? 0.0f : 2048.0f));
    }
}

// Not translated yet: these stop the program so a missing path is never silent.
#ifndef PROG0_HAVE_PLIGHT
void PointLighting(Vu1State&, Batch&) { NotYet("__PLIGHTING"); }
#endif
#ifndef PROG0_HAVE_MLIGHT
void MaterialLighting(Vu1State&, Batch&) { NotYet("__LIGHTINGM_REAL"); }
#endif
#ifndef PROG0_HAVE_CLIP
bool ClipScissor(Vu1State&, Batch&, void (*)(const Vec4*)) { NotYet("scissor path"); }
#endif
#ifndef PROG0_HAVE_BLEND
void VertexBlend(Vu1State&, Batch&) { NotYet("__VERTEX_BLEND"); }
#endif
#ifndef PROG0_HAVE_REFSP
void RefSphere(Vu1State&, Batch&) { NotYet("__REF_SPHERE"); }
#endif

} // namespace prog0

void VuProgModel(Vu1State& vu, void (*xgkick)(const Vec4* packet))
{
    using namespace prog0;
    Vec4* m = vu.mem;
    vu.out_buffer = (vu.out_buffer == 0x1A4) ? 0x258 : 0x1A4; // __INIT_DATA
    const uint16_t in = vu.top;
    Batch b;
    b.tag = &m[in];
    b.n = word(m[in], 0) & 0x7FFF;
    b.type = word(m[in + 1], 3);
    b.pos = &m[in + 2];
    b.colour = b.pos + word(m[in + 1], 0);
    b.uv = b.colour + word(m[in + 1], 1);
    b.vcol = b.uv + word(m[in + 1], 2);

    const uint32_t flags = word(m[0x26], 0);
    if (flags & 0x100) RefSphere(vu, b);
    if (flags & 0x40) VertexBlend(vu, b);

    if ((b.type & 0x200) || (flags & 0x20)) LightingOff(vu, b);
    else if (flags & 0x10) PointLighting(vu, b);
    else if (flags & 0x08) MaterialLighting(vu, b);
    else Lighting(vu, b);

    // The scissor path clips the batch first (and may send part of it); the
    // rest continues through __TRANSPERS like an ordinary batch.
    if ((flags & 0x02) && !ClipScissor(vu, b, xgkick)) return;

    TransPers(vu, b);
    Vec4* out = &m[vu.out_buffer];
    if (flags & 0x04) {
        if ((b.type & 7) == 4) OutputCullStrip(vu, b, out);
        else OutputCull(vu, b, out);
    } else if (flags & 0x01) {
        OutputClip(vu, b, out);
    } else {
        Output(vu, b, out);
    }
    xgkick(out);
}
