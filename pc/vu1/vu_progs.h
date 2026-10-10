// Shared types for the C++ versions of the Dark Cloud 2 VU1 microprograms.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

struct Vec4 { float x, y, z, w; };

struct Vu1State {
    Vec4 mem[1024];          // VU1 data memory
    Vec4 model_screen[4];    // vf10-vf13, left by Vu_progmain for the overlay
    Vec4 light_model[3];     // vf02-vf04, light directions in model space (normalised)
    Vec4 light_color[5];     // vf05-vf09: qw 0x0F-0x12 (4 light colours), 0x13 (ambient)
    uint16_t out_buffer;     // vi13, GIF output buffer: 0x1A4 or 0x258
    uint16_t top;            // vi12, set by the VIF (BASE/OFFSET double buffer)
};

inline uint32_t& word(Vec4& q, int i) { return reinterpret_cast<uint32_t*>(&q)[i]; }
inline uint32_t word(const Vec4& q, int i) { uint32_t w; std::memcpy(&w, &q.x + i, 4); return w; }

// FTOIn: truncate and saturate. Out of range gives INT32_MAX/INT32_MIN on the
// VU; a plain cast gives 0x80000000 for both on x86 (real case: Z of vertices
// far behind the near plane in the first gameplay save state).
inline int32_t ftoi_sat(double s)
{
    return s >= 2147483647.0 ? INT32_MAX : s <= -2147483648.0 ? INT32_MIN : static_cast<int32_t>(s);
}
inline int32_t ftoi4(float f) { return ftoi_sat(double(f) * 16.0); }
inline int32_t ftoi0(float f) { return ftoi_sat(f); }

void VuProgMainSetup(Vu1State& vu);
void VuProgWater(Vu1State& vu, void (*xgkick)(const Vec4* packet));
void VuProg3DSprite(Vu1State& vu, void (*xgkick)(const Vec4* packet));
void VuProgShadow(Vu1State& vu, void (*xgkick)(const Vec4* packet));
void VuProgModel(Vu1State& vu, void (*xgkick)(const Vec4* packet));
