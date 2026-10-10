// C++ reference for the Dark Cloud 2 (PAL) VU1 microprogram Vu_prog_sdw
// (real-time shadows as shadow volumes), written from Vu_prog_sdw.vsm in this
// folder.
//
// Status: bit-exact against the original microcode run in vu1_interp.cpp
// (test_sdw.cpp: 300 objects, 2.434 triangles, 9.350 face packets, 0 differing
// words in packets, output buffers, work area and Vu_progmain's results).
//
// How the shadow works: every triangle of the shadow mesh is extruded towards
// its projection on the ground (matrix at 0x28). The volume's five faces are
// drawn as 4-vertex triangle fans with flat colour (1,1,1,64) and no Z write.
// Faces turned towards the camera are drawn with the GS ALPHA packet at qw 0x2C
// (Cd + Cs*0x80/128, adds) and faces turned away with the packet at qw 0x2E
// (Cd - Cs*0x80/128, subtracts): what is left in the frame buffer is the shadow
// mask, the same idea as a stencil shadow volume but counted in colour.
//
// VU1 memory, as mgCShadowMDT::CreateRenderInfoPacket uploads it:
//   0x04-0x07 model -> screen (unused here), 0x08-0x0B model -> world
//   0x0C-0x0E x = shadow light direction (the other columns are zero)
//   0x15/0x16 guard band, 0x19-0x1C model -> clip, 0x1D-0x20 clip -> screen
//   0x28-0x2B model -> ground -> clip (shadow projection), uploaded after MSCAL 0
//   0x2C/0x2E GIF A+D packets setting ALPHA_1 (add / subtract)
// Input at vi12 (mgCShadowMDT::CreateFacePacket):
//   +0  GIF tag (TRIANGLE_FAN, regs RGBAQ, XYZF2); +1 x,y = triangle count n
//   +2  n triangles, 3 positions each
// Output: packets at vi13 (front faces) and vi13 + 0x5A (back faces), work
// buffer at 0x320.

#include "vu_progs.h"

namespace {

Vec4 Mul(const Vec4* M, const Vec4& p)
{
    return { M[0].x * p.x + M[1].x * p.y + M[2].x * p.z + M[3].x * p.w,
             M[0].y * p.x + M[1].y * p.y + M[2].y * p.z + M[3].y * p.w,
             M[0].z * p.x + M[1].z * p.y + M[2].z * p.z + M[3].z * p.w,
             M[0].w * p.x + M[1].w * p.y + M[2].w * p.z + M[3].w * p.w };
}

Vec4 Sub(const Vec4& a, const Vec4& b) { return { a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w }; }

// CLIPw.xyz: any of x, y, z beyond +-|w|.
bool Outside(const Vec4& v)
{
    const float w = std::fabs(v.w);
    return v.x > w || v.x < -w || v.y > w || v.y < -w || v.z > w || v.z < -w;
}

// OPMULA/OPMSUB pair: a x b with the VU's operand order (ACC = a.yzx*b.zxy,
// result = ACC - b.yzx*a.zxy).
Vec4 Cross(const Vec4& a, const Vec4& b)
{
    return { a.y * b.z - b.y * a.z, a.z * b.x - b.z * a.x, a.x * b.y - b.x * a.y, 0.0f };
}

// The five faces of the volume for one triangle (a = triangle, b = its
// projection), as the microcode orders them for each orientation.
constexpr int kFacesNeg[5][4] = { {0, 1, 4, 3}, {1, 2, 5, 4}, {2, 0, 3, 5}, {3, 4, 5, 5}, {2, 1, 0, 0} };
constexpr int kFacesPos[5][4] = { {0, 3, 4, 1}, {1, 4, 5, 2}, {2, 5, 3, 0}, {5, 4, 3, 3}, {0, 1, 2, 2} };

} // namespace

void VuProgShadow(Vu1State& vu, void (*xgkick)(const Vec4* packet))
{
    Vec4* m = vu.mem;
    vu.out_buffer = (vu.out_buffer == 0x1A4) ? 0x258 : 0x1A4; // __INIT_DATA
    const uint16_t in = vu.top;
    const uint32_t n = word(m[in + 1], 1);
    const Vec4* tri = &m[in + 2];
    Vec4* out = &m[vu.out_buffer];
    Vec4* back = out + 0x5A;
    Vec4* work = &m[0x320];

    out[0] = m[in];

    // Light direction in clip space: (-0.1 * light0 in model space) through the
    // 3x3 part of model -> clip. Only its direction matters (sign of a dot).
    const Vec4* L = vu.light_model;
    const Vec4 e = { -0.1f * L[0].x, -0.1f * L[1].x, -0.1f * L[2].x, 0.0f };
    const Vec4* C = &m[0x19];
    const Vec4 light = { C[0].x * e.x + C[1].x * e.y + C[2].x * e.z,
                         C[0].y * e.x + C[1].y * e.y + C[2].y * e.z,
                         C[0].z * e.x + C[1].z * e.y + C[2].z * e.z, 0.0f };

    // n == 0 would loop 65536 times (do-while on vi11); the EE never sends it.
    for (uint32_t t = 0; t < n; ++t, tri += 3) {
        Vec4 v[6];
        for (int i = 0; i < 3; ++i) v[3 + i] = Mul(&m[0x28], tri[i]); // projected
        for (int i = 0; i < 3; ++i) v[i] = Mul(&m[0x19], tri[i]);     // triangle

        // Whole volume must be inside the clip volume; there is no clipping.
        if (Outside(v[0]) || Outside(v[1]) || Outside(v[2])) continue;
        if (Outside(v[3]) || Outside(v[4]) || Outside(v[5])) continue;

        // Facing relative to the light, in clip space.
        Vec4 nrm = Cross(Sub(v[2], v[0]), Sub(v[1], v[0]));
        const float d = nrm.x * light.x + nrm.y * light.y + nrm.z * light.z;
        if (!(d > 1.0e-4f || d < -1.0e-4f)) continue; // edge-on to the light
        const auto& faces = std::signbit(d) ? kFacesNeg : kFacesPos;
        for (int f = 0; f < 5; ++f)
            for (int k = 0; k < 4; ++k) work[4 * f + k] = v[faces[f][k]];

        // __OUTPUTS
        Vec4 tag = m[in];
        word(tag, 0) = 0x8004;                // EOP | NLOOP 4
        out[0] = tag;
        Vec4 rgbaq;
        word(rgbaq, 0) = 1; word(rgbaq, 1) = 1; word(rgbaq, 2) = 1; word(rgbaq, 3) = 64;
        xgkick(&m[0x2C]);                     // ALPHA: add

        Vec4* fwd = out;
        Vec4* bwd = back;
        int nfront = 0, nback = 0;
        for (int f = 0; f < 5; ++f) {
            Vec4 p[4];
            for (int k = 0; k < 4; ++k) {
                p[k] = Mul(&m[0x1D], work[4 * f + k]);
                const float q = 1.0f / p[k].w;
                p[k] = { p[k].x * q, p[k].y * q, p[k].z * q, p[k].w * q };
            }
            // Screen-space winding: z of (p2 - p0) x (p1 - p0).
            const Vec4 e1 = Sub(p[1], p[0]), e2 = Sub(p[2], p[0]);
            const float z = e2.x * e1.y - e1.x * e2.y;
            Vec4 xyz[4];
            for (int k = 0; k < 4; ++k) {
                word(xyz[k], 0) = ftoi4(p[k].x); word(xyz[k], 1) = ftoi4(p[k].y);
                word(xyz[k], 2) = ftoi4(p[k].z); word(xyz[k], 3) = ftoi4(p[k].w);
            }
            Vec4* o = std::signbit(z) ? bwd : fwd;
            if (!std::signbit(z)) o[0] = tag;
            for (int k = 0; k < 4; ++k) { o[1 + 2 * k] = rgbaq; o[2 + 2 * k] = xyz[k]; }
            if (std::signbit(z)) {
                bwd += 9; ++nback;
            } else {
                xgkick(fwd);                  // front faces go out at once
                fwd += 9; ++nfront;
            }
        }
        // Back faces are only sent when at least one face was in front.
        if (nfront == 0) continue;
        xgkick(&m[0x2C]);
        if (nback == 0) continue;
        xgkick(&m[0x2E]);                     // ALPHA: subtract
        for (Vec4* b = back; nback > 0; --nback, b += 9) {
            b[0] = tag;
            xgkick(b);
        }
        bwd[0] = tag;                         // the loop stores one tag past the end
        xgkick(&m[0x2E]);
    }
}
