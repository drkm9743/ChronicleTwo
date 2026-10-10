#version 450
#extension GL_GOOGLE_include_directive : require

// The GS pixel pipeline from texture lookup to the alpha and destination
// alpha tests. Values are kept in the GS's 0-255 integer range; blending
// itself is fixed-function, with As / 128 as the second blend source.

#include "gs_common.glsl"

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec3 v_stq;
layout(location = 2) in float v_fog;

layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_blend;

layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(set = 0, binding = 1) uniform sampler2D u_target; // copy of the frame buffer, for DATE

int Wrap(int v, int size, int mode, int lo, int hi) {
    if (mode == 0) {
        return v & (size - 1); // REPEAT
    }
    if (mode == 1) {
        return clamp(v, 0, size - 1); // CLAMP
    }
    if (mode == 2) {
        return clamp(v, lo, hi); // REGION_CLAMP
    }
    return (v & lo) | hi; // REGION_REPEAT: MINU is the mask, MAXU the fixed bits
}

vec4 Fetch(ivec2 c) {
    int flags = pc.tex.z;
    c.x = Wrap(c.x, pc.tex.x, (flags >> FLAG_WMS_SHIFT) & 3, pc.region.x, pc.region.y);
    c.y = Wrap(c.y, pc.tex.y, (flags >> FLAG_WMT_SHIFT) & 3, pc.region.z, pc.region.w);
    c += pc.source.xy;
    c = clamp(c, ivec2(0), textureSize(u_texture, 0) - 1);
    vec4 t = floor(texelFetch(u_texture, c, 0) * 255.0 + 0.5);

    // TEXA: 24-bit texels take TA0, 16-bit ones TA0 or TA1 by their alpha bit;
    // with AEM a black texel is transparent.
    int format = pc.texa.w;
    if (format != 0) {
        bool black = pc.texa.z != 0 && all(equal(t.rgb, vec3(0.0)));
        if (format == 2 && t.a >= 128.0) {
            t.a = float(pc.texa.y);
        } else {
            t.a = black ? 0.0 : float(pc.texa.x);
        }
    }
    return t;
}

vec4 SampleTexture() {
    vec2 size = vec2(pc.tex.xy);
    vec2 uv;
    if ((pc.tex.z & FLAG_FST) != 0) {
        uv = v_stq.xy;
    } else {
        float q = v_stq.z != 0.0 ? v_stq.z : 1.0;
        uv = v_stq.xy / q * size;
    }

    if ((pc.tex.z & FLAG_LINEAR) == 0) {
        return Fetch(ivec2(floor(uv)));
    }

    // Bilinear with the GS's 4-bit weights.
    uv -= 0.5;
    ivec2 i = ivec2(floor(uv));
    vec2 f = floor(fract(uv) * 16.0) / 16.0;
    vec4 t00 = Fetch(i);
    vec4 t10 = Fetch(i + ivec2(1, 0));
    vec4 t01 = Fetch(i + ivec2(0, 1));
    vec4 t11 = Fetch(i + ivec2(1, 1));
    return floor(mix(mix(t00, t10, f.x), mix(t01, t11, f.x), f.y));
}

bool AlphaTest(float a) {
    int ref = pc.test.y;
    int ia = int(a);
    switch (pc.test.x) {
    case 0: return false;      // NEVER
    case 1: return true;       // ALWAYS
    case 2: return ia < ref;   // LESS
    case 3: return ia <= ref;  // LEQUAL
    case 4: return ia == ref;  // EQUAL
    case 5: return ia >= ref;  // GEQUAL
    case 6: return ia > ref;   // GREATER
    default: return ia != ref; // NOTEQUAL
    }
}

void main() {
    int flags = pc.tex.z;
    vec4 c = floor(v_color * 255.0 + 0.5);

    if ((flags & FLAG_TME) != 0) {
        vec4 t = SampleTexture();
        int tfx = (flags >> FLAG_TFX_SHIFT) & 3;
        bool tcc = (flags & FLAG_TCC) != 0;
        vec3 rgb;
        float a;
        if (tfx == 0) { // MODULATE
            rgb = min(floor(c.rgb * t.rgb / 128.0), 255.0);
            a = tcc ? min(floor(c.a * t.a / 128.0), 255.0) : c.a;
        } else if (tfx == 1) { // DECAL
            rgb = t.rgb;
            a = tcc ? t.a : c.a;
        } else if (tfx == 2) { // HIGHLIGHT
            rgb = min(floor(c.rgb * t.rgb / 128.0) + c.a, 255.0);
            a = tcc ? min(t.a + c.a, 255.0) : c.a;
        } else { // HIGHLIGHT2
            rgb = min(floor(c.rgb * t.rgb / 128.0) + c.a, 255.0);
            a = tcc ? t.a : c.a;
        }
        c = vec4(rgb, a);
    }

    if ((flags & FLAG_FGE) != 0) {
        float f = floor(v_fog * 255.0 + 0.5);
        c.rgb = floor((f * c.rgb + (255.0 - f) * pc.fog.rgb) / 256.0);
    }

    if ((flags & FLAG_ATE) != 0) {
        bool pass = AlphaTest(c.a);
        if ((flags & FLAG_AT_INVERT) != 0) {
            pass = !pass;
        }
        if (!pass) {
            discard;
        }
    }

    if ((flags & FLAG_DATE) != 0) {
        float da = texelFetch(u_target, ivec2(gl_FragCoord.xy), 0).a * 255.0;
        bool set = da >= 127.5;
        if (set != ((flags & FLAG_DATM) != 0)) {
            discard;
        }
    }

    o_blend = vec4(min(c.a / 128.0, 1.0));
    float out_a = c.a;
    if ((flags & FLAG_FBA) != 0 && out_a < 128.0) {
        out_a += 128.0;
    }
    o_color = vec4(c.rgb, out_a) / 255.0;
}
