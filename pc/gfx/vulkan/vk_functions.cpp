#include "vk_functions.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gfx {

#define GFX_VK_DEFINE(name) PFN_##name name = nullptr;
GFX_VK_GLOBAL_FUNCTIONS(GFX_VK_DEFINE)
GFX_VK_INSTANCE_FUNCTIONS(GFX_VK_DEFINE)
GFX_VK_DEVICE_FUNCTIONS(GFX_VK_DEFINE)
GFX_VK_SWAPCHAIN_FUNCTIONS(GFX_VK_DEFINE)
#undef GFX_VK_DEFINE

PFN_vkGetInstanceProcAddr LoadSystemVulkan() {
#ifdef _WIN32
    HMODULE module = LoadLibraryA("vulkan-1.dll");
    if (module == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
#else
    void *module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        module = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (module == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(module, "vkGetInstanceProcAddr"));
#endif
}

void LoadGlobalFunctions(PFN_vkGetInstanceProcAddr get) {
#define GFX_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(get(VK_NULL_HANDLE, #name));
    GFX_VK_GLOBAL_FUNCTIONS(GFX_VK_LOAD)
#undef GFX_VK_LOAD
}

void LoadInstanceFunctions(PFN_vkGetInstanceProcAddr get, VkInstance instance) {
#define GFX_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(get(instance, #name));
    GFX_VK_INSTANCE_FUNCTIONS(GFX_VK_LOAD)
#undef GFX_VK_LOAD
}

const char *LoadDeviceFunctions(VkDevice device) {
#define GFX_VK_LOAD(name)                                                    \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); \
    if (name == nullptr) {                                                   \
        return #name;                                                        \
    }
    GFX_VK_DEVICE_FUNCTIONS(GFX_VK_LOAD)
#undef GFX_VK_LOAD
#define GFX_VK_LOAD_OPTIONAL(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    GFX_VK_SWAPCHAIN_FUNCTIONS(GFX_VK_LOAD_OPTIONAL)
#undef GFX_VK_LOAD_OPTIONAL
    return nullptr;
}

} // namespace gfx
