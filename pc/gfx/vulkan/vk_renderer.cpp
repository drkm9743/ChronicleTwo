#include "vk_renderer.h"
#include "vk_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace gfx {

namespace {

const VkFormat     kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
const VkDeviceSize kVertexRingBytes = 16u << 20;
const VkDeviceSize kUploadRingBytes = 32u << 20;
const uint32_t     kDescriptorSetsPerFrame = 4096;
const size_t       kMaxTextures = 1024;

/** Layout of the push constants in shaders/gs_common.glsl. */
struct GsPushConstants {
    float   target[4];
    int32_t tex[4];
    int32_t region[4];
    int32_t test[4];
    float   fog[4];
    int32_t texa[4];
    int32_t source[4];
};

static_assert(sizeof(GsPushConstants) == 112, "push constant layout");

enum ShaderFlags : int32_t {
    FLAG_TME = 1 << 0,
    FLAG_FST = 1 << 1,
    FLAG_TCC = 1 << 2,
    FLAG_TFX_SHIFT = 3,
    FLAG_FGE = 1 << 5,
    FLAG_LINEAR = 1 << 6,
    FLAG_WMS_SHIFT = 7,
    FLAG_WMT_SHIFT = 9,
    FLAG_FBA = 1 << 11,
    FLAG_DATE = 1 << 12,
    FLAG_DATM = 1 << 13,
    FLAG_ATE = 1 << 14,
    FLAG_AT_INVERT = 1 << 15,
};

/** Everything a pipeline is created with, packed into one integer. */
struct PipelineKey {
    uint32_t topology;   // GsTopology
    uint32_t blend;      // 0 off, else 1
    uint32_t src_factor; // VkBlendFactor
    uint32_t dst_factor;
    uint32_t op;         // VkBlendOp
    uint32_t write_mask; // VkColorComponentFlags
    uint32_t has_depth;

    uint64_t Pack() const {
        return uint64_t(topology) | (uint64_t(blend) << 2) | (uint64_t(src_factor) << 3) |
               (uint64_t(dst_factor) << 9) | (uint64_t(op) << 15) | (uint64_t(write_mask) << 18) |
               (uint64_t(has_depth) << 22);
    }

    static PipelineKey Unpack(uint64_t k) {
        PipelineKey p;
        p.topology = k & 3;
        p.blend = (k >> 2) & 1;
        p.src_factor = (k >> 3) & 63;
        p.dst_factor = (k >> 9) & 63;
        p.op = (k >> 15) & 7;
        p.write_mask = (k >> 18) & 15;
        p.has_depth = (k >> 22) & 1;
        return p;
    }
};

/**
 * Turns the GS blend (A - B) * C + D into a fixed-function blend. A, B and D
 * pick Cs, Cd or 0, C picks As, Ad or FIX; As reaches the blender as the
 * second fragment output (As / 128), FIX as the blend constant. Returns false
 * for the forms Vulkan cannot express (Cs * (1 + C)), which are approximated.
 */
bool MapBlend(uint64_t alpha, PipelineKey &key) {
    uint32_t a = Bits(alpha, 0, 2), b = Bits(alpha, 2, 2), c = Bits(alpha, 4, 2), d = Bits(alpha, 6, 2);
    // Coefficients of Cs and Cd: k * C + one, with k in -1..1 and one in 0..1.
    int ks = (a == 0) - (b == 0), ones = d == 0;
    int kd = (a == 1) - (b == 1), oned = d == 1;

    VkBlendFactor factor_c, factor_inv;
    switch (c) {
        case 0:
            factor_c = VK_BLEND_FACTOR_SRC1_ALPHA;
            factor_inv = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
            break;
        case 1: // Ad is stored over 255, not 128: off by a factor of two (not handled yet)
            factor_c = VK_BLEND_FACTOR_DST_ALPHA;
            factor_inv = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            break;
        default:
            factor_c = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            factor_inv = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
            break;
    }

    bool exact = true;
    auto factor = [&](int k, int one, bool &negative) {
        negative = false;
        if (k == 0) {
            return one ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        }
        if (k == 1) {
            if (one) {
                exact = false; // 1 + C
                return VK_BLEND_FACTOR_ONE;
            }
            return factor_c;
        }
        if (one) {
            return factor_inv; // 1 - C
        }
        negative = true; // -C
        return factor_c;
    };
    bool          neg_s, neg_d;
    VkBlendFactor fs = factor(ks, ones, neg_s);
    VkBlendFactor fd = factor(kd, oned, neg_d);
    key.blend = 1;
    key.src_factor = fs;
    key.dst_factor = fd;
    key.op = neg_s ? VK_BLEND_OP_REVERSE_SUBTRACT : neg_d ? VK_BLEND_OP_SUBTRACT
                                                          : VK_BLEND_OP_ADD;
    return exact;
}

VkCompareOp DepthCompare(uint32_t ztst) {
    switch (ztst) {
        case 0:
            return VK_COMPARE_OP_NEVER;
        case 1:
            return VK_COMPARE_OP_ALWAYS;
        case 2:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default:
            return VK_COMPARE_OP_GREATER;
    }
}

/** Depth stored as a fraction of the Z format's range, so 24-bit Z is exact in a float. */
double ZScale(uint32_t zpsm) {
    switch (zpsm & 0xF) {
        case 0:
            return 1.0 / 4294967296.0;
        case 1:
            return 1.0 / 16777216.0;
        default:
            return 1.0 / 65536.0;
    }
}

uint64_t Fnv(uint64_t h, const void *data, size_t size) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; i++) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}

bool IsIndexedFormat(uint32_t psm) {
    return psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
}

} // namespace

VulkanRenderer::VulkanRenderer(VulkanContext &context, GsMemory &memory) : ctx_(context), memory_(memory) {}

VulkanRenderer::~VulkanRenderer() {
    if (ctx_.device == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(ctx_.device);
    VkDevice dev = ctx_.device;
    for (auto &t : targets_) {
        DestroyImage(t->image);
    }
    for (auto &kv : textures_) {
        DestroyImage(kv.second.image);
    }
    DestroyImage(dummy_);
    DestroyImage(feedback_);
    DestroyImage(display_);
    DestroyBuffer(readback_);
    for (Frame &f : frames_) {
        for (Image &i : f.graveyard) {
            DestroyImage(i);
        }
        for (Buffer &b : f.buffer_graveyard) {
            DestroyBuffer(b);
        }
        DestroyBuffer(f.vertices);
        DestroyBuffer(f.upload);
        if (f.descriptors) {
            vkDestroyDescriptorPool(dev, f.descriptors, nullptr);
        }
        if (f.fence) {
            vkDestroyFence(dev, f.fence, nullptr);
        }
        if (f.acquired) {
            vkDestroySemaphore(dev, f.acquired, nullptr);
        }
        if (f.pool) {
            vkDestroyCommandPool(dev, f.pool, nullptr);
        }
    }
    for (VkSemaphore s : present_ready_) {
        vkDestroySemaphore(dev, s, nullptr);
    }
    for (auto &kv : pipelines_) {
        vkDestroyPipeline(dev, kv.second, nullptr);
    }
    if (sampler_) {
        vkDestroySampler(dev, sampler_, nullptr);
    }
    if (vertex_shader_) {
        vkDestroyShaderModule(dev, vertex_shader_, nullptr);
    }
    if (fragment_shader_) {
        vkDestroyShaderModule(dev, fragment_shader_, nullptr);
    }
    if (pipeline_layout_) {
        vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr);
    }
    if (set_layout_) {
        vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
    }
}

//
// Resources
//

bool VulkanRenderer::CreateBuffer(Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags props) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(ctx_.device, &bci, nullptr, &buffer.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx_.device, buffer.buffer, &req);
    uint32_t type = ctx_.FindMemoryType(req.memoryTypeBits, props | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX || !(props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        type = ctx_.FindMemoryType(req.memoryTypeBits, props);
    }
    if (type == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(ctx_.device, &mai, nullptr, &buffer.memory) != VK_SUCCESS) {
        return false;
    }
    vkBindBufferMemory(ctx_.device, buffer.buffer, buffer.memory, 0);
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void *p = nullptr;
        vkMapMemory(ctx_.device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &p);
        buffer.mapped = static_cast<uint8_t *>(p);
    }
    buffer.size = size;
    return true;
}

void VulkanRenderer::DestroyBuffer(Buffer &buffer) {
    if (buffer.buffer) {
        vkDestroyBuffer(ctx_.device, buffer.buffer, nullptr);
    }
    if (buffer.memory) {
        vkFreeMemory(ctx_.device, buffer.memory, nullptr);
    }
    buffer = Buffer();
}

bool VulkanRenderer::CreateImage(Image &image, uint32_t width, uint32_t height, VkFormat format,
                                 VkImageUsageFlags usage, VkImageAspectFlags view_aspect) {
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx_.device, &ici, nullptr, &image.image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx_.device, image.image, &req);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = ctx_.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(ctx_.device, &mai, nullptr, &image.memory) != VK_SUCCESS) {
        vkDestroyImage(ctx_.device, image.image, nullptr);
        image = Image();
        return false;
    }
    vkBindImageMemory(ctx_.device, image.image, image.memory, 0);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {view_aspect, 0, 1, 0, 1};
    vkCreateImageView(ctx_.device, &vci, nullptr, &image.view);
    image.width = width;
    image.height = height;
    image.format = format;

    // Every image lives in GENERAL: it is drawn into, sampled and copied in turn.
    EndPass();
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image.image;
    VkImageAspectFlags aspect = view_aspect;
    if (format == ctx_.depth_format) {
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    return true;
}

void VulkanRenderer::DestroyImage(Image &image) {
    if (image.view) {
        vkDestroyImageView(ctx_.device, image.view, nullptr);
    }
    if (image.image) {
        vkDestroyImage(ctx_.device, image.image, nullptr);
    }
    if (image.memory) {
        vkFreeMemory(ctx_.device, image.memory, nullptr);
    }
    image = Image();
}

void VulkanRenderer::RetireImage(Image &image) {
    if (image.image) {
        frames_[frame_index_].graveyard.push_back(image);
    }
    image = Image();
}

bool VulkanRenderer::CreatePipelineObjects(std::string *error) {
    VkDevice dev = ctx_.device;

    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    vkCreateSampler(dev, &sci, nullptr, &sampler_);

    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (uint32_t i = 0; i < 2; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = bindings;
    vkCreateDescriptorSetLayout(dev, &dl, nullptr, &set_layout_);

    VkPushConstantRange        range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                        sizeof(GsPushConstants)};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    vkCreatePipelineLayout(dev, &pl, nullptr, &pipeline_layout_);

    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = kGsVertexShaderSize;
    smi.pCode = kGsVertexShader;
    if (vkCreateShaderModule(dev, &smi, nullptr, &vertex_shader_) != VK_SUCCESS) {
        *error = "could not create the vertex shader";
        return false;
    }
    smi.codeSize = kGsFragmentShaderSize;
    smi.pCode = kGsFragmentShader;
    if (vkCreateShaderModule(dev, &smi, nullptr, &fragment_shader_) != VK_SUCCESS) {
        *error = "could not create the fragment shader";
        return false;
    }
    return true;
}

VkPipeline VulkanRenderer::GetPipeline(uint64_t packed) {
    auto it = pipelines_.find(packed);
    if (it != pipelines_.end()) {
        return it->second;
    }
    PipelineKey key = PipelineKey::Unpack(packed);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription   binding = {0, sizeof(GsVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[5] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(GsVertex, x)   },
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GsVertex, s)   },
        {2, 0, VK_FORMAT_R32_UINT,         offsetof(GsVertex, z)   },
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(GsVertex, rgba)},
        {4, 0, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(GsVertex, fog) },
    };
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = key.topology == TOPOLOGY_POINTS  ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : key.topology == TOPOLOGY_LINES ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                   : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

    VkPipelineColorBlendAttachmentState att = {};
    att.blendEnable = key.blend ? VK_TRUE : VK_FALSE;
    att.srcColorBlendFactor = static_cast<VkBlendFactor>(key.src_factor);
    att.dstColorBlendFactor = static_cast<VkBlendFactor>(key.dst_factor);
    att.colorBlendOp = static_cast<VkBlendOp>(key.op);
    // The GS writes the source alpha as it is; it is never blended.
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    att.alphaBlendOp = VK_BLEND_OP_ADD;
    att.colorWriteMask = key.write_mask;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;

    VkDynamicState dynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,
    };
    VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = sizeof(dynamic) / sizeof(dynamic[0]);
    dyn.pDynamicStates = dynamic;

    VkFormat                      color_format = kColorFormat;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color_format;
    ri.depthAttachmentFormat = key.has_depth ? ctx_.depth_format : VK_FORMAT_UNDEFINED;
    ri.stencilAttachmentFormat = key.has_depth ? ctx_.depth_format : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.pNext = &ri;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &dyn;
    gpi.layout = pipeline_layout_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(ctx_.device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline) != VK_SUCCESS) {
        std::fprintf(stderr, "gfx: pipeline creation failed (key 0x%llx)\n", static_cast<unsigned long long>(packed));
    }
    pipelines_[packed] = pipeline;
    stats_.pipelines++;
    return pipeline;
}

bool VulkanRenderer::Init(std::string *error) {
    std::string unused;
    if (error == nullptr) {
        error = &unused;
    }
    VkDevice dev = ctx_.device;
    for (Frame &f : frames_) {
        VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = ctx_.queue_family;
        vkCreateCommandPool(dev, &cpi, nullptr, &f.pool);
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &cai, &f.cmd);
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev, &fci, nullptr, &f.fence);
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev, &si, nullptr, &f.acquired);
        VkDescriptorPoolSize       size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDescriptorSetsPerFrame * 2};
        VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = kDescriptorSetsPerFrame;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &size;
        vkCreateDescriptorPool(dev, &dpi, nullptr, &f.descriptors);
        VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!CreateBuffer(f.vertices, kVertexRingBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host) ||
            !CreateBuffer(f.upload, kUploadRingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host)) {
            *error = "could not allocate host-visible buffers";
            return false;
        }
    }
    if (!CreatePipelineObjects(error)) {
        return false;
    }
    BeginFrame();

    if (!CreateImage(dummy_, 1, 1, kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT)) {
        *error = "could not create images";
        return false;
    }
    uint32_t black = 0;
    UploadImage(dummy_, 0, 0, 1, 1, &black);
    return true;
}

//
// Command recording
//

void VulkanRenderer::BeginFrame() {
    Frame &f = frames_[frame_index_];
    vkWaitForFences(ctx_.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx_.device, 1, &f.fence);
    for (Image &i : f.graveyard) {
        DestroyImage(i);
    }
    f.graveyard.clear();
    for (Buffer &b : f.buffer_graveyard) {
        DestroyBuffer(b);
    }
    f.buffer_graveyard.clear();
    vkResetCommandPool(ctx_.device, f.pool, 0);
    vkResetDescriptorPool(ctx_.device, f.descriptors, 0);
    f.vertex_used = 0;
    f.upload_used = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    f.recording = true;
    descriptor_sets_used_ = 0;
}

VkCommandBuffer VulkanRenderer::Cmd() { return frames_[frame_index_].cmd; }

void VulkanRenderer::EndPass() {
    if (in_pass_) {
        vkCmdEndRendering(Cmd());
        in_pass_ = false;
        pass_color_ = pass_depth_ = nullptr;
        bound_pipeline_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::Barrier() {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
}

void VulkanRenderer::BeginPass(Target *color, Target *depth) {
    if (in_pass_ && pass_color_ == color && pass_depth_ == depth) {
        return;
    }
    EndPass();
    Barrier();

    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = color->image.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da = ca;
    VkRenderingInfo           ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    uint32_t                  w = color->image.width, h = color->image.height;
    if (depth) {
        da.imageView = depth->image.view;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
        w = std::min(w, depth->image.width);
        h = std::min(h, depth->image.height);
    }
    ri.renderArea = {
        {0, 0},
        {w, h}
    };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(Cmd(), &ri);
    in_pass_ = true;
    pass_color_ = color;
    pass_depth_ = depth;
    bound_pipeline_ = VK_NULL_HANDLE;
}

void VulkanRenderer::SubmitAndWait() {
    EndPass();
    Frame &f = frames_[frame_index_];
    vkEndCommandBuffer(f.cmd);
    f.recording = false;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    vkQueueSubmit(ctx_.queue, 1, &si, f.fence);
    // Everything, including the other frame, so retired images can go.
    vkQueueWaitIdle(ctx_.queue);
    BeginFrame();
}

uint8_t *VulkanRenderer::AllocUpload(VkDeviceSize size, VkDeviceSize *offset) {
    Frame       *f = &frames_[frame_index_];
    VkDeviceSize start = (f->upload_used + 15) & ~VkDeviceSize(15);
    if (start + size > f->upload.size) {
        SubmitAndWait();
        f = &frames_[frame_index_];
        start = 0;
        if (size > f->upload.size) {
            return nullptr;
        }
    }
    f->upload_used = start + size;
    *offset = start;
    return f->upload.mapped + start;
}

void VulkanRenderer::UploadImage(Image &image, int x, int y, int width, int height, const uint32_t *rgba) {
    if (width <= 0 || height <= 0) {
        return;
    }
    VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
    VkDeviceSize offset;
    uint8_t     *p = AllocUpload(bytes, &offset);
    if (p == nullptr) {
        return;
    }
    std::memcpy(p, rgba, bytes);
    EndPass();
    Barrier();
    VkBufferImageCopy region = {};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {x, y, 0};
    region.imageExtent = {uint32_t(width), uint32_t(height), 1};
    vkCmdCopyBufferToImage(Cmd(), frames_[frame_index_].upload.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                           &region);
    Barrier();
}

void VulkanRenderer::UploadDepth(Image &image, int x, int y, int width, int height, const float *depth) {
    if (width <= 0 || height <= 0) {
        return;
    }
    size_t       count = size_t(width) * height;
    VkDeviceSize offset;
    uint8_t     *p = AllocUpload(count * 4, &offset);
    if (p == nullptr) {
        return;
    }
    if (ctx_.depth_format == VK_FORMAT_D24_UNORM_S8_UINT) {
        for (size_t i = 0; i < count; i++) {
            uint32_t v = static_cast<uint32_t>(std::lround(std::min(1.0f, depth[i]) * 16777215.0f));
            std::memcpy(p + i * 4, &v, 4);
        }
    } else {
        std::memcpy(p, depth, count * 4);
    }
    EndPass();
    Barrier();
    VkBufferImageCopy region = {};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.imageOffset = {x, y, 0};
    region.imageExtent = {uint32_t(width), uint32_t(height), 1};
    vkCmdCopyBufferToImage(Cmd(), frames_[frame_index_].upload.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                           &region);
    Barrier();
}

//
// Render targets
//

std::bitset<kGsPageCount> VulkanRenderer::PagesOf(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) const {
    std::bitset<kGsPageCount> pages;
    GsMemory::ForEachPage(bp, bw, psm, rect, [&](uint32_t page) { pages.set(page); });
    return pages;
}

void VulkanRenderer::Reload(Target &t, const GsRect &r) {
    GsRect rect{std::max(r.x0, 0), std::max(r.y0, 0), std::min(r.x1, int(t.image.width)),
                std::min(r.y1, int(t.image.height))};
    if (rect.Empty()) {
        return;
    }
    int w = rect.x1 - rect.x0, h = rect.y1 - rect.y0;
    stats_.target_reloads++;
    if (t.depth) {
        std::vector<float> depth(size_t(w) * h);
        double             scale = ZScale(t.psm);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t z = memory_.ReadPixel(t.psm, t.bp, t.bw, rect.x0 + x, rect.y0 + y);
                depth[size_t(y) * w + x] = static_cast<float>(z * scale);
            }
        }
        UploadDepth(t.image, rect.x0, rect.y0, w, h, depth.data());
    } else {
        std::vector<uint32_t> rgba(size_t(w) * h);
        // A 24-bit buffer still has the top byte in memory; keep it so a later 32-bit view sees it.
        uint32_t read_psm = t.psm == PSMCT24 ? uint32_t(PSMCT32) : t.psm;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                rgba[size_t(y) * w + x] = memory_.ReadColor32(read_psm, t.bp, t.bw, rect.x0 + x, rect.y0 + y);
            }
        }
        UploadImage(t.image, rect.x0, rect.y0, w, h, rgba.data());
    }
}

void VulkanRenderer::ReadBack(Target &t) {
    uint32_t     w = t.image.width, h = t.image.height;
    VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    if (readback_.size < bytes) {
        if (readback_.buffer) {
            frames_[frame_index_].buffer_graveyard.push_back(readback_);
            readback_ = Buffer();
        }
        CreateBuffer(readback_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    EndPass();
    Barrier();
    VkBufferImageCopy region = {};
    region.imageSubresource = {t.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0,
                               0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(Cmd(), t.image.image, VK_IMAGE_LAYOUT_GENERAL, readback_.buffer, 1, &region);
    Barrier();
    SubmitAndWait();

    const uint8_t *data = readback_.mapped;
    if (t.depth) {
        double   inv = 1.0 / ZScale(t.psm);
        uint32_t zmax = ZFormatMax(t.psm);
        bool     d24 = ctx_.depth_format == VK_FORMAT_D24_UNORM_S8_UINT;
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                double f;
                if (d24) {
                    uint32_t v;
                    std::memcpy(&v, data + (size_t(y) * w + x) * 4, 4);
                    f = (v & 0xFFFFFF) / 16777215.0;
                } else {
                    float v;
                    std::memcpy(&v, data + (size_t(y) * w + x) * 4, 4);
                    f = v;
                }
                double   z = std::floor(f * inv + 0.5);
                uint32_t zi = z >= double(zmax) ? zmax : z <= 0 ? 0
                                                                : static_cast<uint32_t>(z);
                memory_.WritePixel(t.psm, t.bp, t.bw, int(x), int(y), zi);
            }
        }
    } else {
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint32_t rgba;
                std::memcpy(&rgba, data + (size_t(y) * w + x) * 4, 4);
                memory_.WriteColor32(t.psm, t.bp, t.bw, int(x), int(y), rgba);
            }
        }
    }
    memory_.MarkWritten(t.bp, t.bw, t.psm, GsRect{0, 0, int(w), int(h)});
    t.gpu_dirty = false;
    stats_.target_readbacks++;
}

void VulkanRenderer::DestroyTarget(size_t index) {
    Target *t = targets_[index].get();
    if (t == pass_color_ || t == pass_depth_) {
        EndPass();
    }
    RetireImage(t->image);
    targets_.erase(targets_.begin() + index);
}

VulkanRenderer::Target *VulkanRenderer::GetTarget(uint32_t bp, uint32_t bw, uint32_t psm, bool depth, uint32_t width,
                                                  uint32_t height) {
    bw = std::max(bw, 1u);
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    for (auto &owned : targets_) {
        Target &t = *owned;
        if (t.bp != bp || t.bw != bw || t.depth != depth || PsmBits(t.psm) != PsmBits(psm)) {
            continue;
        }
        t.last_used = frame_number_;
        if (t.image.width >= width && t.image.height >= height) {
            return &t;
        }
        // Grow: copy what the image holds, load the rest from memory.
        Image    old = t.image;
        uint32_t new_w = std::max(width, old.width), new_h = std::max(height, old.height);
        if (t.gpu_dirty) {
            ReadBack(t);
        }
        if (&t == pass_color_ || &t == pass_depth_) {
            EndPass();
        }
        Image             grown;
        VkFormat          format = depth ? ctx_.depth_format : kColorFormat;
        VkImageUsageFlags usage =
            (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                   : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!CreateImage(grown, new_w, new_h, format, usage,
                         depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT)) {
            return &t;
        }
        t.image = grown;
        RetireImage(old);
        t.pages = PagesOf(bp, bw, psm, GsRect{0, 0, int(new_w), int(new_h)});
        Reload(t, GsRect{0, 0, int(new_w), int(new_h)});
        return &t;
    }

    // A new target. Others over the same memory give it back first and go.
    std::bitset<kGsPageCount> pages = PagesOf(bp, bw, psm, GsRect{0, 0, int(width), int(height)});
    for (size_t i = 0; i < targets_.size();) {
        Target &other = *targets_[i];
        if ((other.pages & pages).any() && &other != pass_color_ && &other != pass_depth_) {
            if (other.gpu_dirty) {
                ReadBack(other);
            }
            DestroyTarget(i);
        } else {
            i++;
        }
    }

    auto    owned = std::make_unique<Target>();
    Target &t = *owned;
    t.bp = bp;
    t.bw = bw;
    t.psm = psm;
    t.depth = depth;
    t.pages = pages;
    t.last_used = frame_number_;
    VkFormat          format = depth ? ctx_.depth_format : kColorFormat;
    VkImageUsageFlags usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateImage(t.image, width, height, format, usage,
                     depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT)) {
        return nullptr;
    }
    Reload(t, GsRect{0, 0, int(width), int(height)});
    targets_.push_back(std::move(owned));
    return targets_.back().get();
}

void VulkanRenderer::SyncToMemory(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) {
    std::bitset<kGsPageCount> pages = PagesOf(bp, bw, psm, rect);
    for (auto &t : targets_) {
        if (t->gpu_dirty && (t->pages & pages).any()) {
            ReadBack(*t);
        }
    }
}

void VulkanRenderer::PrepareWrite(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) {
    std::bitset<kGsPageCount> pages = PagesOf(bp, bw, psm, rect);
    for (auto &t : targets_) {
        if (!t->gpu_dirty || !(t->pages & pages).any()) {
            continue;
        }
        // A write in the target's own layout only replaces the rectangle, which
        // MemoryWritten then uploads; anything else needs the target in memory first.
        bool same_layout = t->bp == bp && t->bw == bw && PsmBits(t->psm) == PsmBits(psm) &&
                           IsZFormat(t->psm) == IsZFormat(psm);
        if (!same_layout) {
            ReadBack(*t);
        }
    }
}

void VulkanRenderer::MemoryWritten(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) {
    std::bitset<kGsPageCount> pages = PagesOf(bp, bw, psm, rect);
    for (auto &t : targets_) {
        if (!(t->pages & pages).any()) {
            continue;
        }
        bool same_layout = t->bp == bp && t->bw == bw && PsmBits(t->psm) == PsmBits(psm) &&
                           IsZFormat(t->psm) == IsZFormat(psm);
        if (same_layout) {
            Reload(*t, rect);
        } else {
            Reload(*t, GsRect{0, 0, int(t->image.width), int(t->image.height)});
            t->gpu_dirty = false;
        }
    }
}

void VulkanRenderer::SyncAll() {
    for (auto &t : targets_) {
        if (t->gpu_dirty) {
            ReadBack(*t);
        }
    }
    SubmitAndWait();
}

//
// Textures
//

VkImageView VulkanRenderer::CopyForSampling(Target &t) {
    if (feedback_.width < t.image.width || feedback_.height < t.image.height) {
        RetireImage(feedback_);
        CreateImage(feedback_, std::max(feedback_.width, t.image.width), std::max(feedback_.height, t.image.height),
                    kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
    }
    EndPass();
    Barrier();
    VkImageCopy region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {t.image.width, t.image.height, 1};
    vkCmdCopyImage(Cmd(), t.image.image, VK_IMAGE_LAYOUT_GENERAL, feedback_.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &region);
    Barrier();
    return feedback_.view;
}

bool VulkanRenderer::ResolveTexture(const GsDrawState &state, const GsClut &clut, Target *color,
                                    TextureBinding *binding, int *alpha_format) {
    uint64_t tex0 = state.tex0;
    uint32_t tbp = Bits(tex0, 0, 14), tbw = Bits(tex0, 14, 6), psm = Bits(tex0, 20, 6);
    uint32_t tw = std::min(Bits(tex0, 26, 4), 10u), th = std::min(Bits(tex0, 30, 4), 10u);
    uint32_t cpsm = Bits(tex0, 51, 4), csa = Bits(tex0, 56, 5);
    int      w = 1 << tw, h = 1 << th;
    bool     indexed = IsIndexedFormat(psm);

    if (psm == PSMCT24 || psm == PSMZ24) {
        *alpha_format = 1;
    } else if (PsmBits(psm) == 16 || (indexed && cpsm != PSMCT32 && cpsm != PSMCT24)) {
        *alpha_format = 2;
    } else {
        *alpha_format = 0;
    }

    // A texture that is (part of) a colour render target is sampled from the image itself.
    if (IsColorFormat(psm)) {
        for (auto &owned : targets_) {
            Target &t = *owned;
            if (t.depth || t.bw != std::max(tbw, 1u) || PsmBits(t.psm) != PsmBits(psm) || tbp < t.bp) {
                continue;
            }
            uint32_t row_blocks = PagesPerRow(t.psm, t.bw) * 32;
            uint32_t delta = tbp - t.bp;
            if (delta % row_blocks != 0) {
                continue;
            }
            int offset_y = static_cast<int>(delta / row_blocks) * PsmPageHeight(t.psm);
            if (offset_y >= int(t.image.height)) {
                continue;
            }
            binding->view = &t == color ? CopyForSampling(t) : t.image.view;
            binding->offset_x = 0;
            binding->offset_y = offset_y;
            t.last_used = frame_number_;
            return true;
        }
    }

    // Otherwise from GS memory, through the cache.
    SyncToMemory(tbp, tbw, psm, GsRect{0, 0, w, h});
    uint64_t key_bits = tex0 & ((uint64_t(1) << 34) - 1); // TBP0, TBW, PSM, TW, TH
    uint64_t key = Fnv(0xCBF29CE484222325ull, &key_bits, sizeof(key_bits));
    if (indexed) {
        uint32_t count = (psm == PSMT8 || psm == PSMT8H) ? 256 : 16;
        uint32_t offset = count == 16 ? csa * 16 : 0;
        uint16_t used[512];
        bool     is32 = cpsm == PSMCT32 || cpsm == PSMCT24;
        for (uint32_t i = 0; i < count; i++) {
            if (is32) {
                uint32_t slot = (offset + i) & 255;
                used[i * 2] = clut.entries[slot];
                used[i * 2 + 1] = clut.entries[slot + 256];
            } else {
                used[i * 2] = clut.entries[(offset + i) & 511];
                used[i * 2 + 1] = 0;
            }
        }
        key = Fnv(key, used, count * 4);
        key = Fnv(key, &cpsm, sizeof(cpsm));
    }

    Texture &tex = textures_[key];
    uint64_t stamp = memory_.RegionStamp(tbp, tbw, psm, GsRect{0, 0, w, h});
    if (tex.image.image == VK_NULL_HANDLE || stamp >= tex.decoded_at) {
        if (tex.image.image == VK_NULL_HANDLE) {
            if (!CreateImage(tex.image, w, h, kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT)) {
                textures_.erase(key);
                return false;
            }
        }
        std::vector<uint32_t> rgba(size_t(w) * h);
        memory_.DecodeTexture(psm, tbp, tbw, w, h, clut, cpsm, csa, rgba.data());
        UploadImage(tex.image, 0, 0, w, h, rgba.data());
        tex.decoded_at = memory_.NextStamp();
        stats_.texture_uploads++;
    }
    tex.last_used = frame_number_;
    binding->view = tex.image.view;
    binding->offset_x = binding->offset_y = 0;
    return true;
}

//
// Drawing
//

void VulkanRenderer::Draw(const GsDrawState &state, const GsVertex *vertices, size_t count, const GsClut &clut) {
    if (count == 0) {
        return;
    }
    uint64_t frame = state.frame, zbuf = state.zbuf, test = state.test;
    uint32_t fbp = Bits(frame, 0, 9) * 32, fbw = Bits(frame, 16, 6), fpsm = Bits(frame, 24, 6);
    uint32_t fbmsk = Bits(frame, 32, 32);
    if (!IsColorFormat(fpsm)) {
        return; // drawing into a depth or indexed layout as colour: not supported yet
    }

    uint32_t sx0 = Bits(state.scissor, 0, 11), sx1 = Bits(state.scissor, 16, 11);
    uint32_t sy0 = Bits(state.scissor, 32, 11), sy1 = Bits(state.scissor, 48, 11);
    uint32_t page_h = PsmPageHeight(fpsm);
    uint32_t width = std::max(fbw, 1u) * 64;
    uint32_t height = ((std::min(sy1 + 1, 2048u) + page_h - 1) / page_h) * page_h;

    bool     ate = test & 1;
    uint32_t atst = Bits(test, 1, 3), aref = Bits(test, 4, 8), afail = Bits(test, 12, 2);
    bool     date = (test >> 14) & 1, datm = (test >> 15) & 1;
    bool     zte = (test >> 16) & 1;
    uint32_t ztst = Bits(test, 17, 2);
    uint32_t zpsm = 0x30 | Bits(zbuf, 24, 4);
    bool     zmsk = (zbuf >> 32) & 1;
    if (zte && ztst == 0) {
        return; // depth test NEVER: nothing is drawn
    }
    bool need_depth = zte && (ztst != 1 || !zmsk);

    Target *color = GetTarget(fbp, fbw, fpsm, false, width, height);
    if (color == nullptr) {
        return;
    }
    Target *depth = nullptr;
    if (need_depth) {
        depth = GetTarget(Bits(zbuf, 0, 9) * 32, fbw, zpsm, true, color->image.width, color->image.height);
        // Making the depth target may have retired an overlapping colour target.
        color = GetTarget(fbp, fbw, fpsm, false, width, height);
        if (color == nullptr) {
            return;
        }
    }

    GsPushConstants pc = {};
    pc.target[0] = 2.0f / color->image.width;
    pc.target[1] = 2.0f / color->image.height;
    pc.target[2] = depth ? static_cast<float>(ZScale(zpsm)) : 0.0f;

    int32_t        flags = 0;
    TextureBinding texture;
    texture.view = dummy_.view;
    if (state.tme) {
        int alpha_format = 0;
        if (!ResolveTexture(state, clut, color, &texture, &alpha_format)) {
            texture.view = dummy_.view;
        }
        uint64_t tex0 = state.tex0, tex1 = state.tex1, clamp = state.clamp, texa = state.texa;
        flags |= FLAG_TME;
        flags |= state.fst ? FLAG_FST : 0;
        flags |= Bits(tex0, 34, 1) ? FLAG_TCC : 0;
        flags |= int32_t(Bits(tex0, 35, 2)) << FLAG_TFX_SHIFT;
        flags |= Bits(tex1, 5, 1) ? FLAG_LINEAR : 0; // MMAG; LOD is not computed yet
        flags |= int32_t(Bits(clamp, 0, 2)) << FLAG_WMS_SHIFT;
        flags |= int32_t(Bits(clamp, 2, 2)) << FLAG_WMT_SHIFT;
        pc.tex[0] = 1 << std::min(Bits(tex0, 26, 4), 10u);
        pc.tex[1] = 1 << std::min(Bits(tex0, 30, 4), 10u);
        pc.region[0] = int32_t(Bits(clamp, 4, 10));
        pc.region[1] = int32_t(Bits(clamp, 14, 10));
        pc.region[2] = int32_t(Bits(clamp, 24, 10));
        pc.region[3] = int32_t(Bits(clamp, 34, 10));
        pc.texa[0] = int32_t(Bits(texa, 0, 8));
        pc.texa[1] = int32_t(Bits(texa, 32, 8));
        pc.texa[2] = int32_t(Bits(texa, 15, 1));
        pc.texa[3] = alpha_format;
        pc.source[0] = texture.offset_x;
        pc.source[1] = texture.offset_y;
    }
    if (state.fge) {
        flags |= FLAG_FGE;
        pc.fog[0] = float(Bits(state.fogcol, 0, 8));
        pc.fog[1] = float(Bits(state.fogcol, 8, 8));
        pc.fog[2] = float(Bits(state.fogcol, 16, 8));
    }
    if (state.fba) {
        flags |= FLAG_FBA;
    }
    pc.test[0] = int32_t(atst);
    pc.test[1] = int32_t(aref);

    VkImageView target_copy = dummy_.view;
    if (date && fpsm != PSMCT24) {
        flags |= FLAG_DATE | (datm ? FLAG_DATM : 0);
        target_copy = CopyForSampling(*color);
    }

    // Colour writes: FBMSK by channel, never alpha for a 24-bit buffer.
    uint32_t write_mask = 0;
    for (int c = 0; c < 4; c++) {
        if (((fbmsk >> (c * 8)) & 0xFF) != 0xFF) {
            write_mask |= 1u << c;
        }
    }
    if (fpsm == PSMCT24) {
        write_mask &= ~uint32_t(VK_COLOR_COMPONENT_A_BIT);
    }

    PipelineKey key = {};
    key.topology = state.topology;
    key.has_depth = depth != nullptr;
    key.src_factor = VK_BLEND_FACTOR_ONE;
    key.dst_factor = VK_BLEND_FACTOR_ZERO;
    key.op = VK_BLEND_OP_ADD;
    if (state.abe) {
        if (!MapBlend(state.alpha, key)) {
            stats_.unsupported_blends++;
        }
    }
    float fix = std::min(Bits(state.alpha, 32, 8) / 128.0f, 1.0f);

    // Two passes when the alpha test keeps some writes for failing pixels (AFAIL).
    struct Pass {
        bool     run;
        int32_t  flags;
        uint32_t write_mask;
        bool     depth_write;
    } passes[2];

    bool depth_write = depth && !zmsk;
    if (!ate || atst == 1) {
        passes[0] = {true, flags, write_mask, depth_write};
        passes[1] = {false, 0, 0, false};
    } else {
        passes[0] = {atst != 0, flags | FLAG_ATE, write_mask, depth_write};
        switch (afail) {
            case 0: // KEEP
                passes[1] = {false, 0, 0, false};
                break;
            case 1: // FB_ONLY
                passes[1] = {true, flags | FLAG_ATE | FLAG_AT_INVERT, write_mask, false};
                break;
            case 2: // ZB_ONLY
                passes[1] = {depth_write, flags | FLAG_ATE | FLAG_AT_INVERT, 0, depth_write};
                break;
            default: // RGB_ONLY
                passes[1] = {true, flags | FLAG_ATE | FLAG_AT_INVERT,
                             write_mask & ~uint32_t(VK_COLOR_COMPONENT_A_BIT), false};
                break;
        }
    }

    // Space for the vertices and a descriptor set; if the frame ran out, flush it first.
    Frame       *f = &frames_[frame_index_];
    VkDeviceSize bytes = VkDeviceSize(count) * sizeof(GsVertex);
    if (bytes > kVertexRingBytes) {
        // Split huge batches on primitive boundaries.
        size_t per = state.topology == TOPOLOGY_TRIANGLES ? 3 : state.topology == TOPOLOGY_LINES ? 2
                                                                                                 : 1;
        size_t chunk = (kVertexRingBytes / sizeof(GsVertex) / 2) / per * per;
        for (size_t i = 0; i < count; i += chunk) {
            Draw(state, vertices + i, std::min(chunk, count - i), clut);
        }
        return;
    }
    if (f->vertex_used + bytes > kVertexRingBytes || descriptor_sets_used_ >= kDescriptorSetsPerFrame) {
        SubmitAndWait();
        f = &frames_[frame_index_];
    }
    VkDeviceSize vertex_offset = f->vertex_used;
    std::memcpy(f->vertices.mapped + vertex_offset, vertices, bytes);
    f->vertex_used += (bytes + 15) & ~VkDeviceSize(15);

    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = f->descriptors;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(ctx_.device, &dai, &set);
    descriptor_sets_used_++;
    VkDescriptorImageInfo images[2] = {
        {sampler_, texture.view, VK_IMAGE_LAYOUT_GENERAL},
        {sampler_, target_copy,  VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(ctx_.device, 2, writes, 0, nullptr);

    BeginPass(color, depth);
    VkCommandBuffer cmd = Cmd();

    VkViewport viewport = {0.0f, 0.0f, float(color->image.width), float(color->image.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    int32_t x0 = int32_t(sx0), y0 = int32_t(sy0);
    int32_t x1 = std::min(int32_t(sx1) + 1, int32_t(color->image.width));
    int32_t y1 = std::min(int32_t(sy1) + 1, int32_t(color->image.height));
    if (depth) {
        x1 = std::min(x1, int32_t(depth->image.width));
        y1 = std::min(y1, int32_t(depth->image.height));
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    VkRect2D scissor = {
        {x0,                y0               },
        {uint32_t(x1 - x0), uint32_t(y1 - y0)}
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    float constants[4] = {fix, fix, fix, fix};
    vkCmdSetBlendConstants(cmd, constants);
    vkCmdSetStencilTestEnable(cmd, VK_FALSE);
    vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthTestEnable(cmd, depth ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cmd, zte ? DepthCompare(ztst) : VK_COMPARE_OP_ALWAYS);
    vkCmdBindVertexBuffers(cmd, 0, 1, &f->vertices.buffer, &vertex_offset);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);

    for (const Pass &pass : passes) {
        if (!pass.run) {
            continue;
        }
        key.write_mask = pass.write_mask;
        VkPipeline pipeline = GetPipeline(key.Pack());
        if (pipeline == VK_NULL_HANDLE) {
            continue;
        }
        if (pipeline != bound_pipeline_) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound_pipeline_ = pipeline;
        }
        pc.tex[2] = pass.flags;
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);
        vkCmdSetDepthWriteEnable(cmd, pass.depth_write ? VK_TRUE : VK_FALSE);
        vkCmdDraw(cmd, uint32_t(count), 1, 0, 0);
        if (pass.write_mask) {
            color->gpu_dirty = true;
        }
        if (pass.depth_write) {
            depth->gpu_dirty = true;
        }
    }
    stats_.draws++;
    stats_.vertices += count;
}

//
// Presentation
//

void VulkanRenderer::VSync(const GsDisplayState &display) {
    Present(display);
    frame_number_++;
    stats_.frames++;

    // Textures nobody used for a while go once the cache is full.
    if (textures_.size() > kMaxTextures) {
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (it->second.last_used + 2 < frame_number_) {
                RetireImage(it->second.image);
                it = textures_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void VulkanRenderer::Present(const GsDisplayState &display) {
    EndPass();

    // Read circuit 2 when it is on (or nothing was set), else circuit 1.
    bool     en1 = display.pmode & 1, en2 = (display.pmode >> 1) & 1;
    int      circuit = (en2 || !en1) ? 1 : 0;
    uint64_t dispfb = display.dispfb[circuit], disp = display.display[circuit];
    uint32_t fbp = Bits(dispfb, 0, 9) * 32, fbw = Bits(dispfb, 9, 6), psm = Bits(dispfb, 15, 5);
    int      dbx = int(Bits(dispfb, 32, 11)), dby = int(Bits(dispfb, 43, 11));
    int      magh = int(Bits(disp, 23, 4)) + 1, magv = int(Bits(disp, 27, 2)) + 1;
    int      w = std::min((int(Bits(disp, 32, 12)) + 1) / magh, 2048);
    int      h = std::min((int(Bits(disp, 44, 11)) + 1) / magv, 2048);

    // The source: a render target holding that buffer, or the buffer decoded from memory.
    Image     *source = nullptr;
    VkOffset3D src0 = {dbx, dby, 0};
    for (auto &t : targets_) {
        if (!t->depth && t->bp == fbp && t->bw == std::max(fbw, 1u) && PsmBits(t->psm) == PsmBits(psm)) {
            source = &t->image;
            break;
        }
    }
    bool have_display = w > 0 && h > 0 && fbw > 0;
    if (have_display && source == nullptr) {
        if (display_.width != uint32_t(w) || display_.height != uint32_t(h)) {
            RetireImage(display_);
            CreateImage(display_, w, h, kColorFormat,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
        }
        std::vector<uint32_t> rgba(size_t(w) * h);
        uint32_t              read_psm = IsColorFormat(psm) ? psm : uint32_t(PSMCT32);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                rgba[size_t(y) * w + x] = memory_.ReadColor32(read_psm, fbp, fbw, dbx + x, dby + y);
            }
        }
        UploadImage(display_, 0, 0, w, h, rgba.data());
        source = &display_;
        src0 = {0, 0, 0};
    }

    Frame   &f = frames_[frame_index_];
    uint32_t image_index = 0;
    bool     presenting = ctx_.HasSurface() && ctx_.Acquire(f.acquired, &image_index);
    if (presenting) {
        while (present_ready_.size() < ctx_.SwapchainImageCount()) {
            VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore           s;
            vkCreateSemaphore(ctx_.device, &si, nullptr, &s);
            present_ready_.push_back(s);
        }
        VkImage         swap = ctx_.SwapchainImage(image_index);
        VkExtent2D      extent = ctx_.SwapchainExtent();
        VkCommandBuffer cmd = Cmd();
        Barrier();
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = swap;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        VkClearColorValue       black = {};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        if (have_display && source != nullptr) {
            // Scale to 4:3 inside the window, centred.
            float    aspect = 4.0f / 3.0f;
            uint32_t dw = extent.width, dh = extent.height;
            if (float(dw) / float(dh) > aspect) {
                dw = uint32_t(dh * aspect);
            } else {
                dh = uint32_t(dw / aspect);
            }
            int         ox = int(extent.width - dw) / 2, oy = int(extent.height - dh) / 2;
            int         sx1 = std::min(src0.x + w, int(source->width)), sy1 = std::min(src0.y + h, int(source->height));
            VkImageBlit blit = {};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = src0;
            blit.srcOffsets[1] = {sx1, sy1, 1};
            blit.dstSubresource = blit.srcSubresource;
            blit.dstOffsets[0] = {ox, oy, 0};
            blit.dstOffsets[1] = {ox + int(dw), oy + int(dh), 1};
            if (sx1 > src0.x && sy1 > src0.y) {
                vkCmdBlitImage(cmd, source->image, VK_IMAGE_LAYOUT_GENERAL, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);
            }
        }
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    }

    vkEndCommandBuffer(f.cmd);
    f.recording = false;
    VkSubmitInfo         si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    if (presenting) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &present_ready_[image_index];
    }
    vkQueueSubmit(ctx_.queue, 1, &si, f.fence);
    if (presenting) {
        ctx_.Present(image_index, present_ready_[image_index]);
    }
    frame_index_ = (frame_index_ + 1) % kFrames;
    BeginFrame();
}

} // namespace gfx
