// Vulkan entry points, loaded at run time through vkGetInstanceProcAddr (from
// SDL or from the system loader), so the build needs only the headers and the
// game does not link vulkan-1.lib.

#pragma once

#include <vulkan/vulkan.h>

#define GFX_VK_GLOBAL_FUNCTIONS(X)            \
    X(vkCreateInstance)                       \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)     \
    X(vkEnumerateInstanceVersion)

#define GFX_VK_INSTANCE_FUNCTIONS(X)             \
    X(vkDestroyInstance)                         \
    X(vkEnumeratePhysicalDevices)                \
    X(vkGetPhysicalDeviceProperties)             \
    X(vkGetPhysicalDeviceFeatures)               \
    X(vkGetPhysicalDeviceFeatures2)              \
    X(vkGetPhysicalDeviceQueueFamilyProperties)  \
    X(vkGetPhysicalDeviceMemoryProperties)       \
    X(vkGetPhysicalDeviceFormatProperties)       \
    X(vkEnumerateDeviceExtensionProperties)      \
    X(vkCreateDevice)                            \
    X(vkGetDeviceProcAddr)                       \
    X(vkDestroySurfaceKHR)                       \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)      \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)      \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define GFX_VK_DEVICE_FUNCTIONS(X)    \
    X(vkDestroyDevice)                \
    X(vkGetDeviceQueue)               \
    X(vkDeviceWaitIdle)               \
    X(vkQueueSubmit)                  \
    X(vkQueueWaitIdle)                \
    X(vkCreateCommandPool)            \
    X(vkDestroyCommandPool)           \
    X(vkResetCommandPool)             \
    X(vkAllocateCommandBuffers)       \
    X(vkBeginCommandBuffer)           \
    X(vkEndCommandBuffer)             \
    X(vkCreateFence)                  \
    X(vkDestroyFence)                 \
    X(vkWaitForFences)                \
    X(vkResetFences)                  \
    X(vkCreateSemaphore)              \
    X(vkDestroySemaphore)             \
    X(vkAllocateMemory)               \
    X(vkFreeMemory)                   \
    X(vkMapMemory)                    \
    X(vkUnmapMemory)                  \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateBuffer)                 \
    X(vkDestroyBuffer)                \
    X(vkGetBufferMemoryRequirements)  \
    X(vkBindBufferMemory)             \
    X(vkCreateImage)                  \
    X(vkDestroyImage)                 \
    X(vkGetImageMemoryRequirements)   \
    X(vkBindImageMemory)              \
    X(vkCreateImageView)              \
    X(vkDestroyImageView)             \
    X(vkCreateSampler)                \
    X(vkDestroySampler)               \
    X(vkCreateShaderModule)           \
    X(vkDestroyShaderModule)          \
    X(vkCreateDescriptorSetLayout)    \
    X(vkDestroyDescriptorSetLayout)   \
    X(vkCreatePipelineLayout)         \
    X(vkDestroyPipelineLayout)        \
    X(vkCreateGraphicsPipelines)      \
    X(vkDestroyPipeline)              \
    X(vkCreateDescriptorPool)         \
    X(vkDestroyDescriptorPool)        \
    X(vkResetDescriptorPool)          \
    X(vkAllocateDescriptorSets)       \
    X(vkUpdateDescriptorSets)         \
    X(vkCmdPipelineBarrier)           \
    X(vkCmdBeginRendering)            \
    X(vkCmdEndRendering)              \
    X(vkCmdBindPipeline)              \
    X(vkCmdBindDescriptorSets)        \
    X(vkCmdBindVertexBuffers)         \
    X(vkCmdPushConstants)             \
    X(vkCmdDraw)                      \
    X(vkCmdSetViewport)               \
    X(vkCmdSetScissor)                \
    X(vkCmdSetBlendConstants)         \
    X(vkCmdSetDepthTestEnable)        \
    X(vkCmdSetDepthWriteEnable)       \
    X(vkCmdSetDepthCompareOp)         \
    X(vkCmdSetStencilTestEnable)      \
    X(vkCmdSetStencilOp)              \
    X(vkCmdSetStencilCompareMask)     \
    X(vkCmdSetStencilWriteMask)       \
    X(vkCmdSetStencilReference)       \
    X(vkCmdSetCullMode)               \
    X(vkCmdSetFrontFace)              \
    X(vkCmdCopyBufferToImage)         \
    X(vkCmdCopyImageToBuffer)         \
    X(vkCmdCopyImage)                 \
    X(vkCmdBlitImage)                 \
    X(vkCmdClearColorImage)

/** Present only when VK_KHR_swapchain is enabled (not in headless use). */
#define GFX_VK_SWAPCHAIN_FUNCTIONS(X) \
    X(vkCreateSwapchainKHR)           \
    X(vkDestroySwapchainKHR)          \
    X(vkGetSwapchainImagesKHR)        \
    X(vkAcquireNextImageKHR)          \
    X(vkQueuePresentKHR)

namespace gfx {

#define GFX_VK_DECLARE(name) extern PFN_##name name;
GFX_VK_GLOBAL_FUNCTIONS(GFX_VK_DECLARE)
GFX_VK_INSTANCE_FUNCTIONS(GFX_VK_DECLARE)
GFX_VK_DEVICE_FUNCTIONS(GFX_VK_DECLARE)
GFX_VK_SWAPCHAIN_FUNCTIONS(GFX_VK_DECLARE)
#undef GFX_VK_DECLARE

/** Opens the system's Vulkan loader (vulkan-1.dll, libvulkan.so.1, libvulkan.1.dylib) and returns its vkGetInstanceProcAddr. */
PFN_vkGetInstanceProcAddr LoadSystemVulkan();

void LoadGlobalFunctions(PFN_vkGetInstanceProcAddr get);
void LoadInstanceFunctions(PFN_vkGetInstanceProcAddr get, VkInstance instance);
/** Loads device functions; returns the name of the first one missing, or nullptr. */
const char *LoadDeviceFunctions(VkDevice device);

} // namespace gfx
