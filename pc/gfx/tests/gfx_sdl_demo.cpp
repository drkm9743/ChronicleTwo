// Opens the window and draws a moving scene through the GS emulation, to
// check the swapchain and presentation on a real machine. Esc or closing the
// window quits; --frames N stops after N frames and --dump FILE.ppm writes the
// last frame buffer (for scripted runs).

#include "gfx.h"
#include "vulkan/vk_context.h"
#include "vulkan/vk_renderer.h"
#include "vulkan/vk_window_sdl.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace gfx;

namespace {

uint64_t Xyz(float x, float y, uint32_t z) {
    return uint64_t(int(x * 16) & 0xFFFF) | (uint64_t(int(y * 16) & 0xFFFF) << 16) | (uint64_t(z) << 32);
}

uint64_t Rgbaq(int r, int g, int b, int a) {
    return uint64_t(r | (g << 8) | (b << 16) | (a << 24)) | (uint64_t(0x3F800000) << 32);
}

/** One frame of a 640x448 display at FBP 0, the way the game sets it up. */
void DrawFrame(GsState &gs, int frame) {
    gs.WriteReg(GS_FRAME_1, 0 | (uint64_t(10) << 16) | (uint64_t(PSMCT32) << 24));
    gs.WriteReg(GS_ZBUF_1, 0x8C | (uint64_t(1) << 24)); // PSMZ24 after the frame buffer
    gs.WriteReg(GS_SCISSOR_1, (uint64_t(639) << 16) | (uint64_t(447) << 48));
    gs.WriteReg(GS_XYOFFSET_1, 0);
    gs.WriteReg(GS_TEST_1, 1 | (1 << 1) | (uint64_t(1) << 16) | (uint64_t(1) << 17));
    gs.WriteReg(GS_PRIM, 6);
    gs.WriteReg(GS_RGBAQ, Rgbaq(20, 24, 40, 0x80));
    gs.WriteReg(GS_XYZ2, Xyz(0, 0, 0));
    gs.WriteReg(GS_XYZ2, Xyz(640, 448, 0));

    // A Gouraud triangle turning around the centre, depth-tested.
    gs.WriteReg(GS_TEST_1, 1 | (1 << 1) | (uint64_t(1) << 16) | (uint64_t(2) << 17));
    gs.WriteReg(GS_PRIM, 3 | (1 << 3));
    float a = frame * 0.03f;
    for (int i = 0; i < 3; i++) {
        float t = a + i * 2.0944f;
        gs.WriteReg(GS_RGBAQ, Rgbaq(i == 0 ? 255 : 0, i == 1 ? 255 : 0, i == 2 ? 255 : 0, 0x80));
        gs.WriteReg(GS_XYZ2, Xyz(320 + std::cos(t) * 180, 224 + std::sin(t) * 180, 0x100000));
    }

    // A translucent square sliding over it (ALPHA (Cs - Cd) * As + Cd).
    gs.WriteReg(GS_ALPHA_1, 0x44);
    gs.WriteReg(GS_PRIM, 6 | (1 << 6));
    float x = 320 + std::sin(frame * 0.02f) * 220;
    gs.WriteReg(GS_RGBAQ, Rgbaq(255, 200, 40, 0x50));
    gs.WriteReg(GS_XYZ2, Xyz(x - 60, 164, 0x200000));
    gs.WriteReg(GS_XYZ2, Xyz(x + 60, 284, 0x200000));
}

} // namespace

int main(int argc, char **argv) {
    int         max_frames = -1;
    const char *dump = nullptr;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            max_frames = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--dump") == 0) {
            dump = argv[i + 1];
        }
    }

    SdlVulkanWindow window;
    std::string     error;
    if (!window.Open("ChronicleTwo gfx", 1280, 960, &error)) {
        std::fprintf(stderr, "gfx_sdl_demo: %s\n", error.c_str());
        return 1;
    }
    VulkanContextOptions options;
    window.Configure(options);
    options.validation = std::getenv("GFX_VALIDATION") != nullptr;
    options.vsync = true;
    VulkanContext ctx;
    if (!ctx.Init(options, &error)) {
        std::fprintf(stderr, "gfx_sdl_demo: %s\n", error.c_str());
        return 1;
    }
    std::printf("gfx_sdl_demo: %s, swapchain %ux%u\n", ctx.properties.deviceName, ctx.SwapchainExtent().width,
                ctx.SwapchainExtent().height);

    Machine &machine = GetMachine();
    {
        VulkanRenderer renderer(ctx, machine.memory);
        if (!renderer.Init(&error)) {
            std::fprintf(stderr, "gfx_sdl_demo: %s\n", error.c_str());
            return 1;
        }
        machine.SetRenderer(&renderer);
        GsState &gs = machine.gs;
        // Display circuit 2 on, 640x448 from FBP 0 (as sceGsSetDefDispEnv would set it).
        gs.WritePrivileged(GS_PRIV_PMODE, 2);
        gs.WritePrivileged(GS_PRIV_DISPFB2, uint64_t(10) << 9);
        gs.WritePrivileged(GS_PRIV_DISPLAY2, (uint64_t(3) << 23) | (uint64_t(2559) << 32) | (uint64_t(447) << 44));

        bool running = true;
        for (int frame = 0; running && frame != max_frames; frame++) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) {
                    running = false;
                }
            }
            DrawFrame(gs, frame);
            gs.VSync();
        }
        if (dump) {
            renderer.SyncAll();
            if (FILE *out = std::fopen(dump, "wb")) {
                std::fprintf(out, "P6\n640 448\n255\n");
                for (int y = 0; y < 448; y++) {
                    for (int x = 0; x < 640; x++) {
                        uint32_t      c = machine.memory.ReadColor32(PSMCT32, 0, 10, x, y);
                        unsigned char rgb[3] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16)};
                        std::fwrite(rgb, 1, 3, out);
                    }
                }
                std::fclose(out);
            }
        }
        const VulkanRenderer::Stats &s = renderer.GetStats();
        std::printf("gfx_sdl_demo: %llu frames, %llu draws, %llu pipelines\n", (unsigned long long) s.frames,
                    (unsigned long long) s.draws, (unsigned long long) s.pipelines);
        machine.SetRenderer(nullptr);
    }
    ctx.Shutdown();
    return 0;
}
