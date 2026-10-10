// The Vulkan instance, device and (when there is a window) swapchain the
// renderer draws with. Windowing stays outside: whoever owns the window hands
// in the instance extensions it needs and a function that makes the surface,
// so the same context runs under SDL 3 or headless for tests.

#pragma once

#include "vk_functions.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gfx {

struct VulkanContextOptions {
    /** Loader entry point; null loads the system's Vulkan loader. */
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
    /** Instance extensions the window system needs (SDL_Vulkan_GetInstanceExtensions). */
    std::vector<std::string> instance_extensions;
    /** Makes the window surface; null runs without a window. */
    std::function<VkSurfaceKHR(VkInstance)> create_surface;
    /** Current drawable size of the window, for the swapchain. */
    std::function<void(uint32_t *width, uint32_t *height)> drawable_size;
    /** Enables VK_LAYER_KHRONOS_validation when it is installed. */
    bool validation = false;
    /** Prefer FIFO (tear-free, may block) over MAILBOX/IMMEDIATE. */
    bool vsync = false;
    /** Treats devices as at most this version (tests use 1.2 to take the extension path). */
    uint32_t max_api_version = VK_API_VERSION_1_3;
    /** Prefers the device whose name contains this text (case-insensitive). */
    std::string device_name;
};

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;

    bool Init(const VulkanContextOptions &options, std::string *error);
    void Shutdown();

    VkInstance                       instance = VK_NULL_HANDLE;
    VkPhysicalDevice                 physical_device = VK_NULL_HANDLE;
    VkDevice                         device = VK_NULL_HANDLE;
    VkQueue                          queue = VK_NULL_HANDLE;
    uint32_t                         queue_family = 0;
    VkPhysicalDeviceProperties       properties = {};
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    VkFormat                         depth_format = VK_FORMAT_UNDEFINED;

    /** Index of a memory type with `required` flags among `type_bits`, or UINT32_MAX. */
    uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required) const;

    // Swapchain. All of these do nothing without a window.

    bool HasSurface() const { return surface_ != VK_NULL_HANDLE; }

    /** Gets the next image; recreates the swapchain when the window changed. False: skip this frame. */
    bool Acquire(VkSemaphore signal, uint32_t *index);
    /** Queues the image for display; marks the swapchain for recreation when it is out of date. */
    void Present(uint32_t index, VkSemaphore wait);

    VkImage SwapchainImage(uint32_t index) const { return swapchain_images_[index]; }

    uint32_t SwapchainImageCount() const { return static_cast<uint32_t>(swapchain_images_.size()); }

    VkExtent2D SwapchainExtent() const { return swapchain_extent_; }

    VkFormat SwapchainFormat() const { return swapchain_format_; }

private:
    bool CreateSwapchain();
    void DestroySwapchain();

    VulkanContextOptions options_;
    VkSurfaceKHR         surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR       swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchain_images_;
    VkExtent2D           swapchain_extent_ = {};
    VkFormat             swapchain_format_ = VK_FORMAT_UNDEFINED;
    bool                 swapchain_stale_ = false;
};

} // namespace gfx
