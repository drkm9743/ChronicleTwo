#include "vk_window_sdl.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace gfx {

SdlVulkanWindow::~SdlVulkanWindow() { Close(); }

bool SdlVulkanWindow::Open(const char *title, int width, int height, std::string *error) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        *error = std::string("SDL video: ") + SDL_GetError();
        return false;
    }
    video_started_ = true;
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        *error = std::string("Vulkan loader: ") + SDL_GetError();
        Close();
        return false;
    }
    vulkan_loaded_ = true;
    window_ = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr) {
        *error = std::string("window: ") + SDL_GetError();
        Close();
        return false;
    }
    return true;
}

void SdlVulkanWindow::Close() {
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (vulkan_loaded_) {
        SDL_Vulkan_UnloadLibrary();
        vulkan_loaded_ = false;
    }
    if (video_started_) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        video_started_ = false;
    }
}

void SdlVulkanWindow::Configure(VulkanContextOptions &options) const {
    options.get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    Uint32             count = 0;
    const char *const *names = SDL_Vulkan_GetInstanceExtensions(&count);
    for (Uint32 i = 0; names && i < count; i++) {
        options.instance_extensions.push_back(names[i]);
    }
    SDL_Window *window = window_;
    options.create_surface = [window](VkInstance instance) {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
            return VkSurfaceKHR(VK_NULL_HANDLE);
        }
        return surface;
    };
    options.drawable_size = [window](uint32_t *width, uint32_t *height) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        *width = w > 0 ? uint32_t(w) : 0;
        *height = h > 0 ? uint32_t(h) : 0;
    };
}

} // namespace gfx
