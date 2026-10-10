// The Vulkan renderer behind GsState. Frame and depth buffers the game draws
// into are real images (render targets); textures are decoded from GS memory
// and cached; batches become draws with one uber-shader whose GS state comes
// in push constants. GS memory stays the CPU's copy: render targets write back
// to it only when the CPU reads or partly overwrites what they cover.

#pragma once

#include "../gs/gs_renderer.h"
#include "vk_context.h"

#include <bitset>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gfx {

class VulkanRenderer : public IGsRenderer {
public:
    VulkanRenderer(VulkanContext &context, GsMemory &memory);
    ~VulkanRenderer() override;

    bool Init(std::string *error);

    void Draw(const GsDrawState &state, const GsVertex *vertices, size_t count, const GsClut &clut) override;
    void SyncToMemory(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) override;
    void PrepareWrite(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) override;
    void MemoryWritten(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) override;
    void VSync(const GsDisplayState &display) override;

    /** Writes every render target back to GS memory and waits for the GPU. */
    void SyncAll();

    struct Stats {
        uint64_t draws = 0;
        uint64_t vertices = 0;
        uint64_t texture_uploads = 0;
        uint64_t target_readbacks = 0;
        uint64_t target_reloads = 0;
        uint64_t pipelines = 0;
        uint64_t frames = 0;
        uint64_t unsupported_blends = 0;
    };

    const Stats &GetStats() const { return stats_; }

private:
    struct Image {
        VkImage        image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view = VK_NULL_HANDLE;
        uint32_t       width = 0, height = 0;
        VkFormat       format = VK_FORMAT_UNDEFINED;
    };

    struct Buffer {
        VkBuffer       buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t       *mapped = nullptr;
        VkDeviceSize   size = 0;
    };

    /** A frame or depth buffer: an image standing for a region of GS memory. */
    struct Target {
        uint32_t                  bp = 0, bw = 0, psm = 0;
        bool                      depth = false;
        Image                     image;
        bool                      gpu_dirty = false; // drawn into since GS memory last had its contents
        std::bitset<kGsPageCount> pages;
        uint64_t                  last_used = 0;
    };

    struct Texture {
        Image    image;
        uint64_t decoded_at = 0; // memory stamp when decoded
        uint64_t last_used = 0;
    };

    struct Frame {
        VkCommandPool       pool = VK_NULL_HANDLE;
        VkCommandBuffer     cmd = VK_NULL_HANDLE;
        VkFence             fence = VK_NULL_HANDLE;
        VkSemaphore         acquired = VK_NULL_HANDLE;
        VkDescriptorPool    descriptors = VK_NULL_HANDLE;
        Buffer              vertices;
        Buffer              upload;
        VkDeviceSize        vertex_used = 0;
        VkDeviceSize        upload_used = 0;
        std::vector<Image>  graveyard; // destroyed once this frame's fence signals
        std::vector<Buffer> buffer_graveyard;
        bool                recording = false;
    };

    /** What an image is sampled as in a draw. */
    struct TextureBinding {
        VkImageView view = VK_NULL_HANDLE;
        int         offset_x = 0, offset_y = 0;
    };

    // Resources.
    bool       CreateImage(Image &image, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                           VkImageAspectFlags view_aspect);
    void       DestroyImage(Image &image);
    void       RetireImage(Image &image);
    bool       CreateBuffer(Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props);
    void       DestroyBuffer(Buffer &buffer);
    bool       CreatePipelineObjects(std::string *error);
    VkPipeline GetPipeline(uint64_t key);

    // Command recording.
    void            BeginFrame();
    VkCommandBuffer Cmd();
    void            EndPass();
    void            BeginPass(Target *color, Target *depth);
    void            Barrier();
    void            SubmitAndWait();
    uint8_t        *AllocUpload(VkDeviceSize size, VkDeviceSize *offset);
    void            UploadImage(Image &image, int x, int y, int width, int height, const uint32_t *rgba);
    void            UploadDepth(Image &image, int x, int y, int width, int height, const float *depth);

    // Render targets.
    Target                   *GetTarget(uint32_t bp, uint32_t bw, uint32_t psm, bool depth, uint32_t width, uint32_t height);
    std::bitset<kGsPageCount> PagesOf(uint32_t bp, uint32_t bw, uint32_t psm, const GsRect &rect) const;
    void                      ReadBack(Target &target);
    void                      Reload(Target &target, const GsRect &rect);
    void                      DestroyTarget(size_t index);

    // Textures.
    bool        ResolveTexture(const GsDrawState &state, const GsClut &clut, Target *color, TextureBinding *binding,
                               int *alpha_format);
    VkImageView CopyForSampling(Target &target);

    // Presentation.
    void Present(const GsDisplayState &display);

    VulkanContext &ctx_;
    GsMemory      &memory_;
    Stats          stats_;

    static const int kFrames = 2;
    Frame            frames_[kFrames];
    int              frame_index_ = 0;
    uint64_t         frame_number_ = 0;

    VkDescriptorSetLayout                    set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout                         pipeline_layout_ = VK_NULL_HANDLE;
    VkShaderModule                           vertex_shader_ = VK_NULL_HANDLE;
    VkShaderModule                           fragment_shader_ = VK_NULL_HANDLE;
    VkSampler                                sampler_ = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkPipeline> pipelines_;

    Image  dummy_;    // 1x1 texture bound when nothing is sampled
    Image  feedback_; // copy of a target that a draw both samples and draws into
    Image  display_;  // display buffer decoded from memory when no target holds it
    Buffer readback_;

    std::vector<std::unique_ptr<Target> > targets_;
    std::unordered_map<uint64_t, Texture> textures_;

    // The pass being recorded.
    bool       in_pass_ = false;
    Target    *pass_color_ = nullptr;
    Target    *pass_depth_ = nullptr;
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    uint32_t   descriptor_sets_used_ = 0;

    std::vector<VkSemaphore> present_ready_; // one per swapchain image
};

} // namespace gfx
