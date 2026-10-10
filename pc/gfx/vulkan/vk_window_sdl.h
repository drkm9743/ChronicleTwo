// The game window on SDL 3, and the glue that lets VulkanContext draw into it:
// the loader SDL found, the instance extensions it needs, the surface and the
// drawable size. Input and audio stay with the rest of the platform layer.

#pragma once

#include "vk_context.h"

#include <string>

struct SDL_Window;

namespace gfx {

class SdlVulkanWindow {
public:
    SdlVulkanWindow() = default;
    ~SdlVulkanWindow();
    SdlVulkanWindow(const SdlVulkanWindow &) = delete;
    SdlVulkanWindow &operator=(const SdlVulkanWindow &) = delete;

    /** Starts SDL video, loads the Vulkan loader and opens a resizable window. */
    bool Open(const char *title, int width, int height, std::string *error);
    void Close();

    /** Points `options` at this window: loader, instance extensions, surface and size. */
    void Configure(VulkanContextOptions &options) const;

    SDL_Window *Window() const { return window_; }

private:
    SDL_Window *window_ = nullptr;
    bool        video_started_ = false;
    bool        vulkan_loaded_ = false;
};

} // namespace gfx
