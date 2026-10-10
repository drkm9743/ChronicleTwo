// SPIR-V of the GS uber-shader (shaders/gs.vert, shaders/gs.frag).

#pragma once

#include <cstddef>
#include <cstdint>

namespace gfx {

extern const uint32_t *const kGsVertexShader;
extern const size_t          kGsVertexShaderSize; // bytes
extern const uint32_t *const kGsFragmentShader;
extern const size_t          kGsFragmentShaderSize;

} // namespace gfx
