#include "vk_shaders.h"

namespace gfx {
namespace {
#include "../shaders/gs_frag.spv.h"
#include "../shaders/gs_vert.spv.h"
} // namespace

const uint32_t *const kGsVertexShader = kGs_vert_spv;
const size_t          kGsVertexShaderSize = sizeof(kGs_vert_spv);
const uint32_t *const kGsFragmentShader = kGs_frag_spv;
const size_t          kGsFragmentShaderSize = sizeof(kGs_frag_spv);

} // namespace gfx
