#include "vk_context.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace gfx {

namespace {

bool ContainsIgnoringCase(const char *text, const std::string &part) {
    std::string a = text, b = part;
    for (char &c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (char &c : b) c = char(std::tolower(static_cast<unsigned char>(c)));
    return a.find(b) != std::string::npos;
}

bool HasExtension(const std::vector<VkExtensionProperties> &list, const char *name) {
    for (const VkExtensionProperties &e : list) {
        if (std::strcmp(e.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool Fail(std::string *error, const std::string &message) {
    if (error) {
        *error = message;
    }
    return false;
}

} // namespace

VulkanContext::~VulkanContext() { Shutdown(); }

uint32_t VulkanContext::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required) const {
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VulkanContext::Init(const VulkanContextOptions &options, std::string *error) {
    options_ = options;
    PFN_vkGetInstanceProcAddr get = options.get_instance_proc_addr;
    if (get == nullptr) {
        get = LoadSystemVulkan();
    }
    if (get == nullptr) {
        return Fail(error, "no Vulkan loader found");
    }
    LoadGlobalFunctions(get);
    if (vkCreateInstance == nullptr) {
        return Fail(error, "the Vulkan loader does not provide vkCreateInstance");
    }

    uint32_t api = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) {
        vkEnumerateInstanceVersion(&api);
    }
    if (api < VK_API_VERSION_1_2) {
        return Fail(error, "the Vulkan loader is older than 1.2");
    }

    std::vector<const char *> extensions;
    for (const std::string &e : options.instance_extensions) {
        extensions.push_back(e.c_str());
    }
    // Implementations layered on another API (MoltenVK on macOS) are only
    // listed when the application says it can live with their limits.
    VkInstanceCreateFlags instance_flags = 0;
    {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        if (HasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    }
    std::vector<const char *> layers;
    if (options.validation) {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        for (const VkLayerProperties &l : available) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
            }
        }
    }

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ChronicleTwo";
    app.pEngineName = "ChronicleTwo";
    app.apiVersion = std::min(api, uint32_t(VK_API_VERSION_1_3));
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = instance_flags;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        return Fail(error, "vkCreateInstance failed");
    }
    LoadInstanceFunctions(get, instance);

    if (options.create_surface) {
        surface_ = options.create_surface(instance);
        if (surface_ == VK_NULL_HANDLE) {
            return Fail(error, "could not create the window surface");
        }
    }

    // Pick a device with dynamic rendering, the extended dynamic state and
    // dual-source blending (core in Vulkan 1.3, extensions on 1.2 devices such
    // as MoltenVK's) and one queue for graphics and presenting. Discrete GPUs first.
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
    int         best_score = -1;
    bool        best_core13 = false, best_portability = false;
    std::string rejected;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        uint32_t version = std::min(props.apiVersion, options.max_api_version);
        if (version < VK_API_VERSION_1_2) {
            rejected += std::string(props.deviceName) + ": below Vulkan 1.2; ";
            continue;
        }
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, exts.data());
        bool                      core13 = version >= VK_API_VERSION_1_3;
        bool                      dynamic_rendering = false, extended_dynamic_state = core13;
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if (core13) {
            VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            f2.pNext = &f13;
            vkGetPhysicalDeviceFeatures2(pd, &f2);
            dynamic_rendering = f13.dynamicRendering;
        } else if (HasExtension(exts, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) &&
                   HasExtension(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME)) {
            VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
            VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
            dr.pNext = &eds;
            f2.pNext = &dr;
            vkGetPhysicalDeviceFeatures2(pd, &f2);
            dynamic_rendering = dr.dynamicRendering;
            extended_dynamic_state = eds.extendedDynamicState;
        } else {
            vkGetPhysicalDeviceFeatures2(pd, &f2);
        }
        if (!dynamic_rendering || !extended_dynamic_state || !f2.features.dualSrcBlend) {
            rejected += std::string(props.deviceName) +
                        ": no dynamic rendering, extended dynamic state or dual-source blending; ";
            continue;
        }
        if (surface_ && !HasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }
        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs.data());
        for (uint32_t i = 0; i < qf_count; i++) {
            if (!(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                continue;
            }
            if (surface_) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
                if (!present) {
                    continue;
                }
            }
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                     : 1;
            if (!options.device_name.empty() && ContainsIgnoringCase(props.deviceName, options.device_name)) {
                score += 10;
            }
            if (score > best_score) {
                best_score = score;
                physical_device = pd;
                queue_family = i;
                properties = props;
                best_core13 = core13;
                best_portability = HasExtension(exts, "VK_KHR_portability_subset");
            }
            break;
        }
    }
    if (physical_device == VK_NULL_HANDLE) {
        return Fail(error, "no suitable Vulkan device (" + rejected + ")");
    }
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    // Depth with stencil: D32_SFLOAT_S8_UINT keeps 24-bit GS Z exact; D24 is the fallback.
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(physical_device, f, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((fp.optimalTilingFeatures & need) == need) {
            depth_format = f;
            break;
        }
    }
    if (depth_format == VK_FORMAT_UNDEFINED) {
        return Fail(error, "no usable depth-stencil format");
    }

    float                   priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.dualSrcBlend = VK_TRUE;
    VkPhysicalDeviceVulkan13Features                f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR     dr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    std::vector<const char *> device_extensions;
    if (best_core13) {
        f13.dynamicRendering = VK_TRUE;
        f2.pNext = &f13;
    } else {
        dr.dynamicRendering = VK_TRUE;
        eds.extendedDynamicState = VK_TRUE;
        dr.pNext = &eds;
        f2.pNext = &dr;
        device_extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        device_extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    }
    if (surface_) {
        device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    if (best_portability) {
        device_extensions.push_back("VK_KHR_portability_subset"); // required when offered
    }
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    dci.ppEnabledExtensionNames = device_extensions.data();
    if (vkCreateDevice(physical_device, &dci, nullptr, &device) != VK_SUCCESS) {
        return Fail(error, "vkCreateDevice failed");
    }
    if (const char *missing = LoadDeviceFunctions(device)) {
        return Fail(error, std::string("missing device function ") + missing);
    }
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    if (surface_ && !CreateSwapchain()) {
        return Fail(error, "could not create the swapchain");
    }
    return true;
}

bool VulkanContext::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface_, &caps);

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface_, &count, formats.data());
    if (formats.empty()) {
        return false;
    }
    // The GS's colours go out as they are, so a UNORM (not sRGB) format.
    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR &f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface_, &count, modes.data());
    // The game paces itself to the PAL field rate, so presenting must not
    // block on the monitor's refresh unless vsync was asked for.
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!options_.vsync) {
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                mode = m;
                break;
            }
        }
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        uint32_t w = 640, h = 480;
        if (options_.drawable_size) {
            options_.drawable_size(&w, &h);
        }
        extent.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return false; // minimised
    }

    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && images > caps.maxImageCount) {
        images = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = images;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR created;
    if (vkCreateSwapchainKHR(device, &sci, nullptr, &created) != VK_SUCCESS) {
        return false;
    }
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain_, nullptr);
    }
    swapchain_ = created;
    swapchain_extent_ = extent;
    swapchain_format_ = chosen.format;
    vkGetSwapchainImagesKHR(device, swapchain_, &count, nullptr);
    swapchain_images_.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain_, &count, swapchain_images_.data());
    swapchain_stale_ = false;
    return true;
}

void VulkanContext::DestroySwapchain() {
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    swapchain_images_.clear();
}

bool VulkanContext::Acquire(VkSemaphore signal, uint32_t *index) {
    if (!surface_) {
        return false;
    }
    if (swapchain_stale_ || swapchain_ == VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        if (!CreateSwapchain()) {
            return false;
        }
    }
    VkResult r = vkAcquireNextImageKHR(device, swapchain_, UINT64_MAX, signal, VK_NULL_HANDLE, index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_stale_ = true;
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) {
        swapchain_stale_ = true; // still usable this time
    }
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

void VulkanContext::Present(uint32_t index, VkSemaphore wait) {
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    VkResult r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        swapchain_stale_ = true;
    }
}

void VulkanContext::Shutdown() {
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        DestroySwapchain();
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

} // namespace gfx
