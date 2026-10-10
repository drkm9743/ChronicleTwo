#include "gfx.h"
#include "pc_graphics.h"
#include "vulkan/vk_context.h"
#include "vulkan/vk_renderer.h"
#include "vulkan/vk_window_sdl.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace pc {

namespace {

/** The window is 2x the PS2's 640x448 frame buffer. */
const int kWindowWidth = 1280;
const int kWindowHeight = 896;

struct Graphics {
    gfx::SdlVulkanWindow                 window;
    gfx::VulkanContext                   context;
    std::unique_ptr<gfx::VulkanRenderer> renderer;
    bool                                 started = false;
    Uint64                               field_ns = 20000000; // PAL
    Uint64                               next_field = 0;
};

Graphics &State() {
    static Graphics graphics;
    return graphics;
}

bool StartRenderer(Graphics &g) {
    if (std::getenv("CHRONICLETWO_HEADLESS") != nullptr) {
        return false;
    }
    std::string error;
    if (!g.window.Open("Dark Chronicle", kWindowWidth, kWindowHeight, &error)) {
        std::fprintf(stderr, "graphics: %s\n", error.c_str());
        return false;
    }
    gfx::VulkanContextOptions options;
    g.window.Configure(options);
    options.validation = std::getenv("CHRONICLETWO_VK_VALIDATION") != nullptr;
    // Fields are paced here, so presentation should not block on the display.
    options.vsync = false;
    if (!g.context.Init(options, &error)) {
        std::fprintf(stderr, "graphics: %s\n", error.c_str());
        g.window.Close();
        return false;
    }
    g.renderer.reset(new gfx::VulkanRenderer(g.context, gfx::GetMachine().memory));
    if (!g.renderer->Init(&error)) {
        std::fprintf(stderr, "graphics: %s\n", error.c_str());
        g.renderer.reset();
        g.context.Shutdown();
        g.window.Close();
        return false;
    }
    gfx::GetMachine().SetRenderer(g.renderer.get());
    return true;
}

} // namespace

void GraphicsStart(bool pal) {
    Graphics &g = State();
    g.field_ns = pal ? 20000000 : 16683350;
    if (g.started) {
        return;
    }
    g.started = true;
    if (!StartRenderer(g)) {
        std::fprintf(stderr, "graphics: running without a renderer\n");
    }
    g.next_field = SDL_GetTicksNS() + g.field_ns;
}

void GraphicsShutdown() {
    Graphics &g = State();
    gfx::GetMachine().SetRenderer(nullptr);
    g.renderer.reset();
    g.context.Shutdown();
    g.window.Close();
    g.started = false;
}

void GraphicsPresent() {
    gfx::GetMachine().gs.VSync();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            GraphicsShutdown();
            std::exit(0);
        }
    }
}

void GraphicsVSync() {
    Graphics &g = State();
    GraphicsPresent();

    Uint64 now = SDL_GetTicksNS();
    if (now < g.next_field) {
        SDL_DelayPrecise(g.next_field - now);
        g.next_field += g.field_ns;
    } else {
        // Running late (or stopped in a debugger): start counting again from now.
        g.next_field = now + g.field_ns;
    }
}

gfx::Machine &GraphicsMachine() { return gfx::GetMachine(); }

} // namespace pc
