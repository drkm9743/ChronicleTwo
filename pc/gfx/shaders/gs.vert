#version 450
#extension GL_GOOGLE_include_directive : require

// Vertices arrive in render-target pixels. The GS samples a pixel at its
// top-left corner and Vulkan at its centre, so everything moves half a pixel.

#include "gs_common.glsl"

layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec3 a_stq;
layout(location = 2) in uint a_z;
layout(location = 3) in vec4 a_color; // R8G8B8A8_UNORM
layout(location = 4) in vec4 a_fog;   // R8G8B8A8_UNORM, F in x

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec3 v_stq;
layout(location = 2) out float v_fog;

void main() {
    vec2 p = (a_pos + 0.5) * pc.target.xy - 1.0;
    gl_Position = vec4(p, float(a_z) * pc.target.z, 1.0);
    gl_PointSize = 1.0;
    v_color = a_color;
    v_stq = a_stq;
    v_fog = a_fog.x;
}
