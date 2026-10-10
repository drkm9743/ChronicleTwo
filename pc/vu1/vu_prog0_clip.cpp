// Scissor path of Vu_prog0 (flag 0x02): __CLIPSCIS / __CLIPSCIS_STR, __SCISSOR,
// __PLANE_START, __ZMINUS..__YPLUS, __CLIP_INTER, __INTERPOLATE,
// __SAVE_LAST_LOOP, __LPoutputsc, __END_ALL, plus the tag write at
// __NXmain1 (micro 0x0d5-0x0d9). Written from Vu_prog0.vsm, lines ~1105-1720.
// Build vu_prog0.cpp with -DPROG0_HAVE_CLIP so it drops its stub.
//
// What it does: every triangle of the batch (lists: each 3 vertices; strips:
// each vertex from the third on) is moved to clip space with the matrix at
// qw 0x19-0x1C. A triangle with a vertex outside the clip volume (|c| > |w|)
// that is not wholly outside one plane is clipped against the six planes
// (Sutherland-Hodgman, ping-ponging between buffers at 0x35C and 0x398), and
// the polygon left is projected with qw 0x1D-0x20 and sent on its own: a
// tag-only packet at 0x30B (NLOOP 0; it carries PRIM from qw 0x27) and then
// the vertices at 0x30C. The batch itself is not changed: afterwards it goes
// through __TRANSPERS and __OUTPUT* like any other (the guard-band ADC hides
// the triangles drawn here).
//
// Everything is done in VU memory with the microcode's addresses and in its
// order, because the side effects are visible later: the packet at 0x30D-0x327
// overlaps the constant colours __LIGHTING_OFF writes at 0x320, and the
// microcode reads those colours (for later triangles and for __OUTPUT) after
// the overwrite. Not modelled: the register spills on the stack at vi14
// (0x3EB-0x3FE), which the microcode writes and reads back unchanged.
//
// Status: bit-exact against the microcode in vu1_interp (PCSX2 semantics, built
// with -frounding-math -include rz_global.h): test_prog0_clip.cpp, 50.000 batches,
// ~300.000 clipped polygons, packets and memory 0x30B-0x3D3 identical; 17
// mutations detected. Three PCSX2 rules show here: the S flag is set on -0 (the
// cull test), the VU has no inf/NaN (the interpolated uv.w is always +0), and
// FMAC operands flush denormals (the vertex copy in __CLIP_INTER).

#include "vu_prog0_internal.h"

namespace prog0 {
namespace {

Vec4& At(Vu1State& vu, uint32_t qw) { return vu.mem[qw & 0x3FF]; }

// mulax/madday/maddaz/maddw: column k of M times component k, summed in order.
Vec4 Transform(const Vec4* M, const Vec4& p)
{
    return { M[0].x * p.x + M[1].x * p.y + M[2].x * p.z + M[3].x * p.w,
             M[0].y * p.x + M[1].y * p.y + M[2].y * p.z + M[3].y * p.w,
             M[0].z * p.x + M[1].z * p.y + M[2].z * p.z + M[3].z * p.w,
             M[0].w * p.x + M[1].w * p.y + M[2].w * p.z + M[3].w * p.w };
}

// addx v, vf00x: a register copy through the adder, which turns -0 into +0.
Vec4 Add0(const Vec4& v) { return { v.x + 0.0f, v.y + 0.0f, v.z + 0.0f, v.w + 0.0f }; }

// An FMAC operand as PCSX2 reads it (vuDouble): denormals become signed zero,
// infinities and NaN +-FLT_MAX. Written out because the compiler folds x * 1.0f.
float VuDouble(float x)
{
    uint32_t u;
    std::memcpy(&u, &x, 4);
    if ((u & 0x7F800000) == 0) u &= 0x80000000;
    else if ((u & 0x7F800000) == 0x7F800000) u = (u & 0x80000000) | 0x7F7FFFFF;
    std::memcpy(&x, &u, 4);
    return x;
}

Vec4 ThroughFmac(const Vec4& v) { return { VuDouble(v.x), VuDouble(v.y), VuDouble(v.z), VuDouble(v.w) }; }

float Comp(const Vec4& v, int c) { return c == 0 ? v.x : c == 1 ? v.y : c == 2 ? v.z : v.w; }

// CLIPw.xyz: bit 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z, against |w|.
uint32_t ClipBits(const Vec4& v)
{
    const float w = std::fabs(v.w);
    uint32_t bits = 0;
    for (int f = 0; f < 3; ++f) {
        if (Comp(v, f) > +w) bits |= 1u << (f * 2);
        if (Comp(v, f) < -w) bits |= 1u << (f * 2 + 1);
    }
    return bits;
}

// The __CLIPSCIS test: some vertex is outside (FCAND 0x03FFFF) and the three
// are not all outside the same plane (each FCOR mask leaves one plane's bit of
// the three judgements).
bool NeedsScissor(const Vec4& a, const Vec4& b, const Vec4& c)
{
    const uint32_t flags = ClipBits(a) << 12 | ClipBits(b) << 6 | ClipBits(c);
    if ((flags & 0x03FFFF) == 0) return false;
    static const uint32_t kAllOut[] = { 0xFF7DF7, 0xFFBEFB, 0xFFDF7D, 0xFFEFBE, 0xFDF7DF, 0xFEFBEF };
    for (uint32_t m : kAllOut)
        if ((flags | m) == 0xFFFFFF) return false;
    return true;
}

struct Vertex { Vec4 pos, col, uv; }; // one clip-buffer entry, 3 qw

Vertex Load(Vu1State& vu, uint32_t qw) { return { At(vu, qw), At(vu, qw + 1), At(vu, qw + 2) }; }
void Store(Vu1State& vu, uint32_t qw, const Vertex& v) { At(vu, qw) = v.pos; At(vu, qw + 1) = v.col; At(vu, qw + 2) = v.uv; }

struct Plane { uint32_t cur_mask, next_mask; int comp; float side; uint32_t src, dst; };

// __ZMINUS .. __YPLUS, in the microcode's order. FCGET puts the current
// vertex's judgement in bits 6-11 and the next one's in bits 0-5.
const Plane kPlanes[6] = {
    { 0x800, 0x20, 2, -1.0f, 0x35C, 0x398 },
    { 0x400, 0x10, 2, +1.0f, 0x398, 0x35C },
    { 0x080, 0x02, 0, -1.0f, 0x35C, 0x398 },
    { 0x040, 0x01, 0, +1.0f, 0x398, 0x35C },
    { 0x200, 0x08, 1, -1.0f, 0x35C, 0x398 },
    { 0x100, 0x04, 1, +1.0f, 0x398, 0x35C },
};

// __INTERPOLATE, from the current vertex a towards the next one b. The new
// vertex's fourth texture word is vf27.w - vf27.w: +0, since the VU has no
// infinities or NaN (PCSX2 clamps them to +-FLT_MAX).
Vertex Interpolate(const Vertex& a, const Vertex& b, const Plane& pl)
{
    // mulx.w + subw: distance to the plane w = side * c, then MR32 brings
    // component `comp` to x.
    const float da = Comp(a.pos, pl.comp) - a.pos.w * pl.side;
    const float db = Comp(b.pos, pl.comp) - b.pos.w * pl.side;
    const float t = std::fabs(0.0f + da / (db - da));
    Vertex r;
    r.pos = { (b.pos.x - a.pos.x) * t + a.pos.x, (b.pos.y - a.pos.y) * t + a.pos.y,
              (b.pos.z - a.pos.z) * t + a.pos.z, (b.pos.w - a.pos.w) * t + a.pos.w };
    r.col = { (b.col.x - a.col.x) * t + a.col.x, (b.col.y - a.col.y) * t + a.col.y,
              (b.col.z - a.col.z) * t + a.col.z, (b.col.w - a.col.w) * t + a.col.w };
    r.uv = { (b.uv.x - a.uv.x) * t + a.uv.x, (b.uv.y - a.uv.y) * t + a.uv.y,
             (b.uv.z - a.uv.z) * t + a.uv.z, 0.0f };
    return r;
}

// __SCISSOR for one triangle: clip-space positions p (vf18-vf20), colours and
// texture coordinates at col and uv (vi04, vi05).
void Scissor(Vu1State& vu, const Vec4 p[3], uint32_t col, uint32_t uv, void (*xgkick)(const Vec4*))
{
    const Vec4 c[3] = { At(vu, col), At(vu, col + 1), At(vu, col + 2) };
    const Vec4 t[3] = { At(vu, uv), At(vu, uv + 1), At(vu, uv + 2) };
    for (int k = 0; k < 4; ++k) At(vu, 0x35C + 3 * k) = p[k % 3];
    for (int k = 0; k < 4; ++k) At(vu, 0x35C + 3 * k + 1) = c[k % 3];
    for (int k = 0; k < 4; ++k) At(vu, 0x35C + 3 * k + 2) = t[k % 3];

    int count = 3; // vi11
    for (const Plane& pl : kPlanes) {
        const int iters = count;
        count = 0;
        uint32_t src = pl.src, dst = pl.dst;
        Vertex next = Load(vu, src);
        src += 3;
        for (int i = 0; i < iters; ++i) {
            // __CLIP_INTER: mulw by vf00w (1.0) copies the current vertex
            // through the FMAC, which flushes denormals to signed zero (PCSX2
            // vuDouble). It matters for colours __LIGHTING_OFF keeps at 0x320
            // once the packet has overwritten them with XYZF2 integers.
            const Vertex cur = { ThroughFmac(next.pos), ThroughFmac(next.col), ThroughFmac(next.uv) };
            next = Load(vu, src);
            src += 3;
            const uint32_t flags = ClipBits(cur.pos) << 6 | ClipBits(next.pos);
            const bool cur_out = flags & pl.cur_mask, next_out = flags & pl.next_mask;
            if (!cur_out) { Store(vu, dst, cur); dst += 3; ++count; }
            if (cur_out != next_out) {
                const Vertex v = Interpolate(cur, next, pl);
                Store(vu, dst, v); dst += 3; ++count;
            }
        }
        // __SAVE_LAST_LOOP: close the polygon with a copy of its first vertex.
        Store(vu, dst, Load(vu, pl.dst));
        if (count == 0) return;
    }

    // Output (0x5a1-0x5e8). The result is in 0x35C after __YPLUS.
    const Vec4 S[4] = { At(vu, 0x1D), At(vu, 0x1E), At(vu, 0x1F), At(vu, 0x20) };
    const Vec4 tag = At(vu, 0x27);
    Vec4& prim = At(vu, 0x30B);
    word(prim, 0) = 0x8000;
    for (int i = 1; i < 4; ++i) word(prim, i) = word(tag, i);
    xgkick(&prim);
    Vec4& head = At(vu, 0x30C);
    word(head, 0) = (0x8000u + count) & 0xFFFF;
    for (int i = 1; i < 4; ++i) word(head, i) = word(tag, i);
    const Vec4 fog = At(vu, 0x3B);
    for (int k = 0; k < count; ++k) {
        const Vertex v = Load(vu, 0x35C + 3 * k);
        Vec4 s = Transform(S, v.pos);
        const float q = 1.0f / s.w;
        s = { s.x * q, s.y * q, s.z * q, fog.x + fog.w * q };
        const Vec4 st = { v.uv.x * q, v.uv.y * q, v.uv.z * q, v.uv.w };
        
        s.w = std::min(s.w, fog.y);
        At(vu, 0x35C + 3 * k) = s; // stored between MINIy and MAXz
        s.w = std::max(s.w, fog.z);
        At(vu, 0x30D + 3 * k) = st;
        At(vu, 0x30E + 3 * k) = ToI0(v.col);
        At(vu, 0x30F + 3 * k) = ToI4(s);
    }
    // Back-face cull (flag 0x04) on the first three projected vertices: the
    // sign of z of (v2 - v0) x (v1 - v0) must equal qw 0x1E.x (0 or 0x20).
    if (word(At(vu, 0x26), 0) & 0x4) {
        const Vec4 p0 = At(vu, 0x35C), p1 = At(vu, 0x35F), p2 = At(vu, 0x362);
        const Vec4 e1 = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z, p1.w - p0.w };
        const Vec4 e2 = { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z, p2.w - p0.w };
        const float z = e2.x * e1.y - e1.x * e2.y;
        const uint32_t sign = std::signbit(z) ? 0x20 : 0; // PCSX2 sets S on -0 too
        if (sign != (word(At(vu, 0x1E), 0) & 0xFFFF)) return;
    }
    xgkick(&head);
}

void SwapEnds(Vu1State& vu, uint32_t col, uint32_t uv)
{
    const Vec4 c0 = At(vu, col), c2 = At(vu, col + 2), t0 = At(vu, uv), t2 = At(vu, uv + 2);
    At(vu, col + 2) = c0; At(vu, col) = c2;
    At(vu, uv + 2) = t0; At(vu, uv) = t2;
}

} // namespace

bool ClipScissor(Vu1State& vu, Batch& b, void (*xgkick)(const Vec4* packet))
{
    if (b.n == 0) return false; // the microcode would loop on vi09 wrapping
    const Vec4 C[4] = { vu.mem[0x19], vu.mem[0x1A], vu.mem[0x1B], vu.mem[0x1C] };
    uint32_t pos = uint32_t(b.pos - vu.mem), col = uint32_t(b.colour - vu.mem), uv = uint32_t(b.uv - vu.mem);
    int16_t left = int16_t(b.n); // vi09

    if ((b.type & 7) != 4) {
        // __CLIPSCIS: triangle lists. The next triangle's positions are read
        // before the current one is clipped.
        Vec4 p[3] = { At(vu, pos), At(vu, pos + 1), At(vu, pos + 2) };
        do {
            const Vec4 c[3] = { Transform(C, p[0]), Transform(C, p[1]), Transform(C, p[2]) };
            for (int k = 0; k < 3; ++k) p[k] = At(vu, pos + 3 + k);
            left = int16_t(left - 3);
            if (NeedsScissor(c[0], c[1], c[2])) Scissor(vu, c, col, uv, xgkick);
            pos += 3; col += 3; uv += 3;
        } while (left != 0);
    } else {
        // __CLIPSCIS_STR: one triangle per vertex from the third on. Odd
        // triangles are clipped with their first and last vertices swapped
        // (registers and, for colours and texture coordinates, memory) so the
        // winding stays the same; both are swapped back afterwards.
        Vec4 r[3]; // vf18, vf19, vf20
        r[0] = Transform(C, At(vu, pos));
        r[1] = Transform(C, At(vu, pos + 1));
        left = int16_t(left - 2);
        for (uint32_t parity = 0;; ++parity) {
            r[2] = Transform(C, At(vu, pos + 2));
            if (NeedsScissor(r[0], r[1], r[2])) {
                if (parity & 1) {
                    Vec4 t = Add0(r[2]); r[2] = Add0(r[0]); r[0] = Add0(t);
                    SwapEnds(vu, col, uv);
                    Scissor(vu, r, col, uv, xgkick);
                    t = Add0(r[2]); r[2] = Add0(r[0]); r[0] = Add0(t);
                    SwapEnds(vu, col, uv);
                } else {
                    Scissor(vu, r, col, uv, xgkick);
                }
            }
            left = int16_t(left - 1);
            r[0] = Add0(r[1]);
            r[1] = Add0(r[2]);
            ++pos; ++col; ++uv;
            if (left == 0) break;
        }
    }

    // __NXmain1: vi11 > 0, so the microcode writes a tag at 0x30C (EOP, NLOOP 0,
    // rest from qw 0x27) before going on to __TRANSPERS.
    Vec4& t = vu.mem[0x30C];
    word(t, 0) = 0x8000;
    for (int i = 1; i < 4; ++i) word(t, i) = word(vu.mem[0x27], i);
    return true;
}

} // namespace prog0
