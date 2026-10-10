// Vu_prog0: __VERTEX_BLEND (flag 0x40, skinning) and __REF_SPHERE (flag 0x100,
// sphere-map reflection). Written from Vu_prog0.vsm; checked in test_prog0.cpp.
#include "vu_prog0_internal.h"

namespace prog0 {

// __VERTEX_BLEND. After the vertex colours comes one more stream, 2 qw per
// vertex: four bone offsets (integers, quadwords from 0x3C, where
// mgCVisualMotionMDT uploads 4 qw per bone) and four weights. Position and
// normal are rewritten in place as the weighted sum of the four bone matrices
// applied to them (the weight multiplies the input vector first).
//
// The normal uses only the 3x3 part, and for the fourth bone the microcode
// takes column y where column z belongs (micro 0x13e: maddz vf31, vf14, vf28z;
// vf15 holds column z). Kept as is.
void VertexBlend(Vu1State& vu, Batch& b)
{
    Vec4* m = vu.mem;
    const Vec4* bones = b.vcol + word(b.tag[1], 0);
    for (uint32_t i = 0; i < b.n; ++i) {
        const Vec4& idx = bones[2 * i];
        const Vec4& wt = bones[2 * i + 1];
        const Vec4 p = b.pos[i], n = b.colour[i];
        const float w[4] = { wt.x, wt.y, wt.z, wt.w };
        const Vec4* M[4];
        for (int k = 0; k < 4; ++k) M[k] = &m[(0x3C + (word(idx, k) & 0xFFFF)) & 0x3FF];

        Vec4 acc = { 0, 0, 0, 0 };
        auto madd = [&](const Vec4& c, float s, bool first) {
            if (first) acc = { c.x * s, c.y * s, c.z * s, c.w * s };
            else acc = { acc.x + c.x * s, acc.y + c.y * s, acc.z + c.z * s, acc.w + c.w * s };
        };
        for (int k = 0; k < 4; ++k) {
            const Vec4 pw = { p.x * w[k], p.y * w[k], p.z * w[k], p.w * w[k] };
            madd(M[k][0], pw.x, k == 0);
            madd(M[k][1], pw.y, false);
            madd(M[k][2], pw.z, false);
            madd(M[k][3], pw.w, false);
        }
        const Vec4 np = acc;
        for (int k = 0; k < 4; ++k) {
            const Vec4 nw = { n.x * w[k], n.y * w[k], n.z * w[k], n.w * w[k] };
            madd(M[k][0], nw.x, k == 0);
            madd(M[k][1], nw.y, false);
            madd(M[k][k == 3 ? 1 : 2], nw.z, false);
        }
        b.pos[i] = np;
        b.colour[i] = acc;
    }
}

// __REF_SPHERE: texture coordinates from the normal for a sphere map:
// t = (view x model, qw 0x19-0x1B) * n, uv.xy = (t.xy / |t| + 1) / 2.
// Only S and T are written; it runs before skinning, on the model's normals.
void RefSphere(Vu1State& vu, Batch& b)
{
    const Vec4* V = &vu.mem[0x19];
    for (uint32_t i = 0; i < b.n; ++i) {
        const Vec4 n = b.colour[i];
        const Vec4 t = { V[0].x * n.x + V[1].x * n.y + V[2].x * n.z,
                         V[0].y * n.x + V[1].y * n.y + V[2].y * n.z,
                         V[0].z * n.x + V[1].z * n.y + V[2].z * n.z, 0 };
        const float r = 1.0f / std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z); // ERLENG
        b.uv[i].x = (t.x * r + 1.0f) * 0.5f;
        b.uv[i].y = (t.y * r + 1.0f) * 0.5f;
    }
}

} // namespace prog0
