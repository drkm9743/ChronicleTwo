// Vu_prog0 hooks PointLighting (__PLIGHTING, flag 0x10) and MaterialLighting
// (__LIGHTINGM_REAL, flag 0x08), written from Vu_prog0.vsm. Build together
// with vu_prog0.cpp and define PROG0_HAVE_PLIGHT and PROG0_HAVE_MLIGHT so
// that file drops its stubs. Bit-exact against the microcode in vu1_interp
// with its PCSX2 rules (test_prog0_light.cpp); like the interpreter it needs
// round toward zero with FTZ/DAZ: build with -frounding-math -include rz_global.h.
//
// __ADD_PLIGHT and __LIGHTINGC are never called (no BAL or B reaches them),
// so they have no translation.
//
// Both loops are software-pipelined; the order of each float operation below
// follows the microcode (ACC chains are evaluated left to right, one rounding
// per step). Q and P reads were checked against the timing: every MULQ and
// MFP in the loops sees the result meant for the current vertex.

#include "vu_prog0_internal.h"

namespace prog0 {

namespace {

float Bits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
uint32_t Raw(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
const float kFltMax = Bits(0x7F7FFFFF);

// MAX and MINI compare the bit patterns as integers (as PCSX2 does): +0 beats -0.
float FMax(float a, float b)
{
    const int32_t x = int32_t(Raw(a)), y = int32_t(Raw(b));
    return Bits(uint32_t((x < 0 && y < 0) ? std::min(x, y) : std::max(x, y)));
}
float FMin(float a, float b)
{
    const int32_t x = int32_t(Raw(a)), y = int32_t(Raw(b));
    return Bits(uint32_t((x < 0 && y < 0) ? std::max(x, y) : std::min(x, y)));
}
Vec4 Max0(const Vec4& v) { return { FMax(v.x, 0.0f), FMax(v.y, 0.0f), FMax(v.z, 0.0f), FMax(v.w, 0.0f) }; }
Vec4 MinAll(const Vec4& v, float k) { return { FMin(v.x, k), FMin(v.y, k), FMin(v.z, k), FMin(v.w, k) }; }

// RSQRT Q, vf00w, x: 1/sqrt|x|, and +-FLT_MAX (sign of x) for x = 0.
float Rsqrt(float x) { return x == 0.0f ? std::copysign(kFltMax, x) : 1.0f / std::sqrt(std::fabs(x)); }
// ERSQRT P, x: 1/sqrt(x); 0 for 0, and a negative x comes back unchanged.
float Ersqrt(float x)
{
    if (!(x >= 0.0f)) return x;
    const float r = std::sqrt(x);
    return r != 0.0f ? 1.0f / r : r;
}
// ERLENG P, v: 1/|v.xyz|, with the same special cases as ERSQRT.
float Erleng(const Vec4& v) { return Ersqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// mulax/madday/maddz with three rows: r0*v.x + r1*v.y + r2*v.z, per field.
Vec4 Rows3(const Vec4& r0, const Vec4& r1, const Vec4& r2, const Vec4& v)
{
    Vec4 a;
    a.x = r0.x * v.x; a.x += r1.x * v.y; a.x += r2.x * v.z;
    a.y = r0.y * v.x; a.y += r1.y * v.y; a.y += r2.y * v.z;
    a.z = r0.z * v.x; a.z += r1.z * v.y; a.z += r2.z * v.z;
    a.w = r0.w * v.x; a.w += r1.w * v.y; a.w += r2.w * v.z;
    return a;
}

// mulaw base, vf00w; maddax..maddw with four colours weighted by k.
Vec4 Sum4(const Vec4& base, const Vec4* C, const Vec4& k)
{
    Vec4 a;
    a.x = base.x * 1.0f; a.x += C[0].x * k.x; a.x += C[1].x * k.y; a.x += C[2].x * k.z; a.x += C[3].x * k.w;
    a.y = base.y * 1.0f; a.y += C[0].y * k.x; a.y += C[1].y * k.y; a.y += C[2].y * k.z; a.y += C[3].y * k.w;
    a.z = base.z * 1.0f; a.z += C[0].z * k.x; a.z += C[1].z * k.y; a.z += C[2].z * k.z; a.z += C[3].z * k.w;
    a.w = base.w * 1.0f; a.w += C[0].w * k.x; a.w += C[1].w * k.y; a.w += C[2].w * k.z; a.w += C[3].w * k.w;
    return a;
}

} // namespace

// __PLIGHTING: directional lighting as __LIGHTING plus 4 point lights.
// qw 0x2D-0x30: light position minus the model's translation (world axes),
// w = power; qw 0x31-0x34: light colours. Both are scaled by the material
// diffuse colour (qw 0x28). Normals and positions are rotated into world axes
// by the rows at qw 0x08-0x0A (no translation). Per light c, with d the vector
// from the vertex to the light and n the rotated normal:
//   f = max(d.n, 0) / (|d| |n|) * min(power^2 / |d|^2, 1)
// The microcode multiplies many of these terms by vf01, which Vu_progmain sets
// to (1, 1, 1, 1); those products are left out. 1/|d| comes from ERSQRT for
// lights 0 and 2 and from RSQRT for lights 1 and 3 (their special cases differ
// at d = 0, where f is 0 either way).
void PointLighting(Vu1State& vu, Batch& b)
{
    Vec4* m = vu.mem;
    const Vec4 mat = m[0x28];
    Vec4 dirc[5], pc[4];
    for (int k = 0; k < 5; ++k) m[0x320 + k] = dirc[k] = Mul(vu.light_color[k], mat); // spilled at 800-808
    for (int k = 0; k < 4; ++k) m[0x325 + k] = pc[k] = Mul(m[0x31 + k], mat);
    const Vec4* L = &m[0x2D];
    const Vec4 pw = { L[0].w, L[1].w, L[2].w, L[3].w };
    const Vec4 pw2 = Mul(pw, pw);
    const Vec4 W0 = m[0x08], W1 = m[0x09], W2 = m[0x0A];
    const Vec4* lm = vu.light_model;

    for (uint32_t i = 0; i < b.n; ++i) {
        const Vec4 nr = b.colour[i];
        const Vec4 n = Rows3(W0, W1, W2, nr);
        const Vec4 p = Rows3(W0, W1, W2, b.pos[i]);
        // vf10-vf12: x, y and z of the 4 light vectors, one light per field.
        const Vec4 X = { L[0].x - p.x, L[1].x - p.x, L[2].x - p.x, L[3].x - p.x };
        const Vec4 Y = { L[0].y - p.y, L[1].y - p.y, L[2].y - p.y, L[3].y - p.y };
        const Vec4 Z = { L[0].z - p.z, L[1].z - p.z, L[2].z - p.z, L[3].z - p.z };
        Vec4 dist2 = Mul(X, X);
        dist2.x += Y.x * Y.x; dist2.x += Z.x * Z.x;
        dist2.y += Y.y * Y.y; dist2.y += Z.y * Z.y;
        dist2.z += Y.z * Y.z; dist2.z += Z.z * Z.z;
        dist2.w += Y.w * Y.w; dist2.w += Z.w * Z.w;
        const Vec4 dot = Max0(Rows3(X, Y, Z, n));

        const Vec4 n2 = Mul(n, n);
        const float inv_n = Rsqrt(n2.x + n2.y + n2.z);
        const Vec4 inv_d = { Ersqrt(dist2.x), Rsqrt(dist2.y), Ersqrt(dist2.z), Rsqrt(dist2.w) };
        const Vec4 cosv = { inv_d.x * inv_n, inv_d.y * inv_n, inv_d.z * inv_n, inv_d.w * inv_n };
        const Vec4 att = MinAll(Mul(Mul(pw2, inv_d), inv_d), 1.0f);
        const Vec4 f = Mul(Mul(dot, cosv), att);

        const Vec4 e = Max0(Rows3(lm[0], lm[1], lm[2], nr));
        const Vec4 dir = Sum4(dirc[4], dirc, e);
        b.colour[i] = MinAll(Sum4(dir, pc, f), kMaxColour);
    }
    if (b.type & 0x100) VertexColours(vu, b);
}

// __LIGHTINGM_REAL: __LIGHTING (diffuse colours scaled by qw 0x28) plus a
// Phong specular term per directional light, colours scaled by qw 0x29.
// v is the vector from the vertex to the eye (qw 0x18, model space), l the
// light direction (vf02-vf04 columns):
//   r.v = 2 (n.l)(n.v) - l.v,  spec = (max(r.v, 0) / |v|) ^ (2 ^ (k + 1))
// with k = qw 0x2B.x (mat_pw, 3 in the game), and the specular term is cut
// where n.l < 1e-4 (scaled linearly below that).
void MaterialLighting(Vu1State& vu, Batch& b)
{
    const Vec4* m = vu.mem;
    const Vec4 dmat = m[0x28], smat = m[0x29], eye = m[0x18];
    Vec4 dc[5], sc[4];
    for (int k = 0; k < 4; ++k) sc[k] = Mul(vu.light_color[k], smat);
    for (int k = 0; k < 5; ++k) dc[k] = Mul(vu.light_color[k], dmat);
    // ILW gives 16 bits; the squaring loop runs until the counter wraps to 0.
    const uint16_t k = static_cast<uint16_t>(word(m[0x2B], 0));
    const uint32_t squarings = k ? k : 0x10000;
    const float eps = Bits(0x38D1B717), inv_eps = Bits(0x461C4000); // 1e-4, 1e4
    const Vec4* lm = vu.light_model;

    for (uint32_t i = 0; i < b.n; ++i) {
        const Vec4 n = b.colour[i], p = b.pos[i];
        const Vec4 v = { eye.x - p.x, eye.y - p.y, eye.z - p.z, eye.w - p.w };
        const Vec4 nl = Rows3(lm[0], lm[1], lm[2], n);
        const Vec4 lv = Rows3(lm[0], lm[1], lm[2], v);
        const float inv_v = Erleng(v);
        Vec4 h = Mul(v, n);
        h = { h.x + h.x, h.y + h.y, h.z + h.z, h.w + h.w };
        const Vec4 diff = Max0(nl);

        Vec4 rv;
        rv.x = nl.x * h.x; rv.x += nl.x * h.y; rv.x += nl.x * h.z; rv.x -= lv.x * 1.0f;
        rv.y = nl.y * h.x; rv.y += nl.y * h.y; rv.y += nl.y * h.z; rv.y -= lv.y * 1.0f;
        rv.z = nl.z * h.x; rv.z += nl.z * h.y; rv.z += nl.z * h.z; rv.z -= lv.z * 1.0f;
        rv.w = nl.w * h.x; rv.w += nl.w * h.y; rv.w += nl.w * h.z; rv.w -= lv.w * 1.0f;
        Vec4 spec = Max0(rv);
        spec = { spec.x * inv_v, spec.y * inv_v, spec.z * inv_v, spec.w * inv_v };
        spec = Mul(spec, spec);
        for (uint32_t r = 0; r < squarings; ++r) {
            const Vec4 prev = spec;
            spec = Mul(spec, spec);
            if (std::memcmp(&prev, &spec, sizeof spec) == 0) break; // 0, 1 or FLT_MAX: nothing changes any more
        }
        Vec4 mask = MinAll(diff, eps);
        mask = Max0({ mask.x * inv_eps, mask.y * inv_eps, mask.z * inv_eps, mask.w * inv_eps });

        const Vec4 d = Sum4(dc[4], dc, diff);
        b.colour[i] = MinAll(Sum4(d, sc, Mul(spec, mask)), kMaxColour);
    }
    if (b.type & 0x100) VertexColours(vu, b);
}

} // namespace prog0
