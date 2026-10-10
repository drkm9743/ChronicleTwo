// Push constants shared by gs.vert and gs.frag. Keep in step with
// GsPushConstants in vulkan/vk_renderer.cpp.

layout(push_constant) uniform Push {
    vec4 target;   // 2 / width, 2 / height, Z scale, unused
    ivec4 tex;     // texture width, height (2^TW, 2^TH), flags, unused
    ivec4 region;  // CLAMP MINU, MAXU, MINV, MAXV
    ivec4 test;    // ATST, AREF, unused, unused
    vec4 fog;      // FOGCOL red, green, blue (0-255), unused
    ivec4 texa;    // TA0, TA1, AEM, alpha format (0: as stored, 1: 24-bit, 2: 16-bit)
    ivec4 source;  // offset of the texture inside its image (x, y), unused, unused
} pc;

// pc.tex.z bits
const int FLAG_TME = 1 << 0;
const int FLAG_FST = 1 << 1;
const int FLAG_TCC = 1 << 2;
const int FLAG_TFX_SHIFT = 3;   // 2 bits
const int FLAG_FGE = 1 << 5;
const int FLAG_LINEAR = 1 << 6;
const int FLAG_WMS_SHIFT = 7;   // 2 bits
const int FLAG_WMT_SHIFT = 9;   // 2 bits
const int FLAG_FBA = 1 << 11;
const int FLAG_DATE = 1 << 12;
const int FLAG_DATM = 1 << 13;
const int FLAG_ATE = 1 << 14;
const int FLAG_AT_INVERT = 1 << 15;
