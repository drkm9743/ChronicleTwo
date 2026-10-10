#include "gfx.h"
#include "pc_graphics.h"
#include "vulkan/vk_context.h"
#include "vulkan/vk_renderer.h"
#include "vulkan/vk_window_sdl.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace pc {

extern uint64_t dma_chains_sent[10];

namespace {

/**
 * CHRONICLETWO_TRACE: passes everything on to the real renderer (or none)
 * and once a second prints what reached the GS, so a black window can be
 * told apart from a game that draws nothing.
 */
class TraceRenderer : public gfx::IGsRenderer {
public:
    gfx::IGsRenderer *inner = nullptr;

    void Draw(const gfx::GsDrawState &state, const gfx::GsVertex *vertices, size_t count,
              const gfx::GsClut &clut) override {
        draws_++;
        vertices_ += count;
        if (state.tme) {
            textured_++;
        }
        last_frame_ = state.frame;
        last_state_ = state;
        if (count > 0) {
            last_vertex_ = vertices[0];
        }
        if (inner) {
            inner->Draw(state, vertices, count, clut);
        }
    }
    void SyncToMemory(uint32_t bp, uint32_t bw, uint32_t psm, const gfx::GsRect &rect) override {
        if (inner) {
            inner->SyncToMemory(bp, bw, psm, rect);
        }
    }
    void PrepareWrite(uint32_t bp, uint32_t bw, uint32_t psm, const gfx::GsRect &rect) override {
        if (inner) {
            inner->PrepareWrite(bp, bw, psm, rect);
        }
    }
    void MemoryWritten(uint32_t bp, uint32_t bw, uint32_t psm, const gfx::GsRect &rect) override {
        uploads_++;
        if (inner) {
            inner->MemoryWritten(bp, bw, psm, rect);
        }
    }
    void VSync(const gfx::GsDisplayState &display) override {
        if (vsyncs_ % 50 == 0) {
            DescribeDisplay(display);
        }
        if (vsyncs_++ % 50 == 0) {
            std::fprintf(stderr,
                         "trace: vsync %llu draws %llu vertices %llu uploads %llu last FRAME %016llx | dma vif1 %llu "
                         "gif %llu spr %llu | PMODE %llx DISPFB1 %016llx DISPLAY1 %016llx DISPFB2 %016llx "
                         "DISPLAY2 %016llx\n",
                         (unsigned long long) vsyncs_, (unsigned long long) draws_, (unsigned long long) vertices_,
                         (unsigned long long) uploads_, (unsigned long long) last_frame_,
                         (unsigned long long) dma_chains_sent[1], (unsigned long long) dma_chains_sent[2],
                         (unsigned long long) dma_chains_sent[8], (unsigned long long) display.pmode,
                         (unsigned long long) display.dispfb[0], (unsigned long long) display.display[0],
                         (unsigned long long) display.dispfb[1], (unsigned long long) display.display[1]);
        }
        if (inner) {
            inner->VSync(display);
        }
    }

private:
    /** Writes the displayed buffer back to GS memory and counts the pixels that are not black. */
    void DescribeDisplay(const gfx::GsDisplayState &display) {
        bool     en1 = display.pmode & 1, en2 = (display.pmode >> 1) & 1;
        int      circuit = (en2 || !en1) ? 1 : 0;
        uint64_t dispfb = display.dispfb[circuit], disp = display.display[circuit];
        uint32_t fbp = uint32_t(dispfb & 0x1FF) * 32, fbw = uint32_t((dispfb >> 9) & 0x3F);
        uint32_t psm = uint32_t((dispfb >> 15) & 0x1F);
        int      magh = int((disp >> 23) & 0xF) + 1, magv = int((disp >> 27) & 3) + 1;
        int      w = std::min(int(((disp >> 32) & 0xFFF) + 1) / magh, 2048);
        int      h = std::min(int(((disp >> 44) & 0x7FF) + 1) / magv, 2048);
        if (fbw == 0 || w <= 0 || h <= 0) {
            return;
        }
        if (inner) {
            inner->SyncToMemory(fbp, fbw, psm, gfx::GsRect{0, 0, w, h});
        }
        std::fprintf(stderr,
                     "trace: display circuit %d bp %u bw %u psm %u %dx%d: %d of %d sampled pixels not black; "
                     "%llu textured draws\n",
                     circuit + 1, fbp, fbw, psm, w, h, CountLit(fbp, fbw, psm, w, h), ((w + 1) / 2) * ((h + 1) / 2),
                     (unsigned long long) textured_);
        Dump("display", fbp, fbw, psm, w, h);
        // The buffer drawn last, and how.
        uint32_t dbp = uint32_t(last_frame_ & 0x1FF) * 32, dbw = uint32_t((last_frame_ >> 16) & 0x3F);
        uint32_t dpsm = uint32_t((last_frame_ >> 24) & 0x3F);
        if (dbw != 0) {
            if (inner) {
                inner->SyncToMemory(dbp, dbw, dpsm, gfx::GsRect{0, 0, w, h});
            }
            std::fprintf(stderr, "trace: drawn bp %u bw %u psm %u: %d sampled pixels not black\n", dbp, dbw, dpsm,
                         CountLit(dbp, dbw, dpsm, w, h));
            Dump("drawn", dbp, dbw, dpsm, w, h);
        }
        const gfx::GsDrawState &d = last_state_;
        const gfx::GsVertex    &v = last_vertex_;
        std::fprintf(stderr,
                     "trace: last draw topology %u tme %u abe %u fst %u test %llx alpha %llx scissor %llx zbuf %llx "
                     "tex0 %llx | vertex %.1f,%.1f z %u st %.3f,%.3f q %.3f rgba %08x\n",
                     d.topology, d.tme, d.abe, d.fst, (unsigned long long) d.test, (unsigned long long) d.alpha,
                     (unsigned long long) d.scissor, (unsigned long long) d.zbuf, (unsigned long long) d.tex0, v.x, v.y,
                     v.z, v.s, v.t, v.q, v.rgba);
    }

    /** CHRONICLETWO_TRACE_DUMP=<folder>: the buffer as a BMP there, once a second. It stays on this PC. */
    void Dump(const char *what, uint32_t bp, uint32_t bw, uint32_t psm, int w, int h) {
        const char *folder = std::getenv("CHRONICLETWO_TRACE_DUMP");
        if (folder == nullptr) {
            return;
        }
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/%s_%05llu.bmp", folder, what, (unsigned long long) vsyncs_);
        FILE *f = std::fopen(path, "wb");
        if (f == nullptr) {
            return;
        }
        uint32_t row = uint32_t(w) * 3 + 3 & ~3u, image = row * uint32_t(h);
        uint8_t  header[54] = {'B', 'M'};
        auto     put32 = [&](int at, uint32_t v) {
            for (int i = 0; i < 4; i++) {
                header[at + i] = uint8_t(v >> (8 * i));
            }
        };
        put32(2, 54 + image);
        put32(10, 54);
        put32(14, 40);
        put32(18, uint32_t(w));
        put32(22, uint32_t(h));
        header[26] = 1;
        header[28] = 24;
        put32(34, image);
        std::fwrite(header, 1, sizeof(header), f);
        gfx::GsMemory       &memory = gfx::GetMachine().memory;
        std::vector<uint8_t> line(row);
        for (int y = h - 1; y >= 0; y--) {
            for (int x = 0; x < w; x++) {
                uint32_t c = memory.ReadColor32(psm, bp, bw, x, y);
                line[x * 3 + 0] = uint8_t(c >> 16);
                line[x * 3 + 1] = uint8_t(c >> 8);
                line[x * 3 + 2] = uint8_t(c);
            }
            std::fwrite(line.data(), 1, row, f);
        }
        std::fclose(f);
    }

    static int CountLit(uint32_t bp, uint32_t bw, uint32_t psm, int w, int h) {
        gfx::GsMemory &memory = gfx::GetMachine().memory;
        int            lit = 0;
        for (int y = 0; y < h; y += 2) {
            for (int x = 0; x < w; x += 2) {
                if (memory.ReadColor32(psm, bp, bw, x, y) & 0xFFFFFF) {
                    lit++;
                }
            }
        }
        return lit;
    }

    uint64_t         vsyncs_ = 0, draws_ = 0, vertices_ = 0, uploads_ = 0, last_frame_ = 0;
    uint64_t         textured_ = 0;
    gfx::GsDrawState last_state_ = {};
    gfx::GsVertex    last_vertex_ = {};
};

/** The window is 2x the PS2's 640x448 frame buffer. */
const int kWindowWidth = 1280;
const int kWindowHeight = 896;

struct Graphics {
    gfx::SdlVulkanWindow                 window;
    gfx::VulkanContext                   context;
    std::unique_ptr<gfx::VulkanRenderer> renderer;
    TraceRenderer                        trace;
    bool                                 tracing = false;
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
    if (const char *gpu = std::getenv("CHRONICLETWO_GPU")) {
        options.device_name = gpu;
    }
    // Fields are paced here, so presentation should not block on the display.
    options.vsync = false;
    if (!g.context.Init(options, &error)) {
        std::fprintf(stderr, "graphics: %s\n", error.c_str());
        g.window.Close();
        return false;
    }
    std::fprintf(stderr, "graphics: %s\n", g.context.properties.deviceName);
    if (!g.context.rejected_devices.empty()) {
        std::fprintf(stderr, "graphics: passed over %s\n", g.context.rejected_devices.c_str());
    }
    g.renderer.reset(new gfx::VulkanRenderer(g.context, gfx::GetMachine().memory));
    if (!g.renderer->Init(&error)) {
        std::fprintf(stderr, "graphics: %s\n", error.c_str());
        g.renderer.reset();
        g.context.Shutdown();
        g.window.Close();
        return false;
    }
    g.trace.inner = g.renderer.get();
    gfx::GetMachine().SetRenderer(g.tracing ? static_cast<gfx::IGsRenderer *>(&g.trace) : g.renderer.get());
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
    g.tracing = std::getenv("CHRONICLETWO_TRACE") != nullptr;
    if (!StartRenderer(g)) {
        std::fprintf(stderr, "graphics: running without a renderer\n");
        if (g.tracing) {
            gfx::GetMachine().SetRenderer(&g.trace);
        }
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
