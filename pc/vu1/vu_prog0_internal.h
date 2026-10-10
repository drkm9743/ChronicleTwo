// Internal interface of the C++ Vu_prog0 (vu_prog0.cpp): the batch layout and
// helpers shared by the subroutine files. Each subroutine of the microcode
// becomes a function that reads and writes the batch in VU1 memory, exactly
// as the microcode does, so the pieces can be written and tested separately.
//
// Hooks still to be written (vu_prog0.cpp calls them; each lives in its own
// file and has its own test against vu1_interp, modelled on test_prog0.cpp):
//   PointLighting      __PLIGHTING + __ADD_PLIGHT   (flag 0x10)
//   MaterialLighting   __LIGHTINGM_REAL (+ __LIGHTINGC) (flag 0x08)
//   ClipScissor        __CLIPSCIS / __CLIPSCIS_STR, __SCISSOR, __ZMINUS..__YPLUS,
//                      __CLIP_INTER and their output (flag 0x02)
// Done: VertexBlend (__VERTEX_BLEND, flag 0x40) and RefSphere (__REF_SPHERE,
// flag 0x100) in vu_prog0_blend.cpp.
#pragma once
#include "vu_progs.h"

#include <cstdio>
#include <cstdlib>

namespace prog0 {

[[noreturn]] inline void NotYet(const char* what)
{
    std::fprintf(stderr, "Vu_prog0: %s is not translated yet\n", what);
    std::abort();
}

inline Vec4 MinY(Vec4 v, float lim)
{
    return { std::min(v.x, lim), std::min(v.y, lim), std::min(v.z, lim), std::min(v.w, lim) };
}

inline Vec4 Mul(const Vec4& a, const Vec4& b) { return { a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w }; }

inline Vec4 ToI0(const Vec4& v)
{
    Vec4 r;
    word(r, 0) = ftoi0(v.x); word(r, 1) = ftoi0(v.y);
    word(r, 2) = ftoi0(v.z); word(r, 3) = ftoi0(v.w);
    return r;
}

inline Vec4 ToI4(const Vec4& v)
{
    Vec4 r;
    word(r, 0) = ftoi4(v.x); word(r, 1) = ftoi4(v.y);
    word(r, 2) = ftoi4(v.z); word(r, 3) = ftoi4(v.w);
    return r;
}

constexpr float kMaxColour = 255.0f; // vf14.y, set by Vu_progmain

struct Batch {
    Vec4* tag;
    uint32_t n;        // vi11: vertices (NLOOP of the input tag)
    uint32_t type;     // header w
    Vec4* pos;         // vi02
    Vec4* colour;      // vi04: normals, then colours after lighting
    Vec4* uv;          // vi05
    Vec4* vcol;        // vi06: vertex colours
};

struct Guard { Vec4 max, min; };

// Translated subroutines (vu_prog0.cpp).
void VertexColours(Vu1State& vu, Batch& b);   // __LIGHTING_VC
void LightingOff(Vu1State& vu, Batch& b);     // __LIGHTING_OFF
void Lighting(Vu1State& vu, Batch& b);        // __LIGHTING
void TransPers(Vu1State& vu, Batch& b);       // __TRANSPERS
void Output(Vu1State& vu, Batch& b, Vec4* out);          // __OUTPUT
void OutputClip(Vu1State& vu, Batch& b, Vec4* out);      // __OUTPUTC
void OutputCull(Vu1State& vu, Batch& b, Vec4* out);      // __OUTPUTCN
void OutputCullStrip(Vu1State& vu, Batch& b, Vec4* out); // __OUTPUTCN_STR
float Fog(const Vec4& fog, float q);
Guard LoadGuard(const Vec4* m);
bool Inside(const Guard& g, const Vec4& p);

// Hooks (see the list at the top). ClipScissor returns false when nothing is
// left to draw (vi11 <= 0 after clipping).
void PointLighting(Vu1State& vu, Batch& b);
void MaterialLighting(Vu1State& vu, Batch& b);
bool ClipScissor(Vu1State& vu, Batch& b, void (*xgkick)(const Vec4* packet));
void VertexBlend(Vu1State& vu, Batch& b);
void RefSphere(Vu1State& vu, Batch& b);

} // namespace prog0
