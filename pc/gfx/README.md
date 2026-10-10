# pc/gfx

The graphics side of the PS2 for the PC build: what the game sends to DMA
channels 1 and 2 goes through VIF1, VU1 and the GIF into an emulated GS whose
drawing runs on Vulkan.

```
game -> libdma / libpkt / libgraph shims (pc/src/sce)
     -> Dma (dma/)  -> Vif1 (vif/) -> Vu1Host -> microprograms (C++)
                    -> Gif (gif/)  <- XGKICK / DIRECT
     -> GsState (gs/)  registers, vertex queue, transfers, CLUT, GS memory
     -> IGsRenderer    VulkanRenderer (vulkan/), or nothing
```

* `gs/` keeps the 4 MiB of GS memory with the real page/block/column layout,
  so transfers, CLUT loads and textures read exactly what the game wrote.
  Primitives leave it as batches of `GsVertex` with the GS state they need.
* `vulkan/` draws those batches. Frame and Z buffers are images (render
  targets) tied to a region of GS memory; they are written back only when the
  CPU reads that region or overwrites it in another layout. Textures are
  decoded from GS memory and cached until their pages are written. One
  uber-shader covers texture functions, TEXA, fog, alpha test and DATE; the
  GS blend becomes dual-source blending.
* `vif/vu1_host.*` runs VU1 microprograms translated to C++: each is
  registered by the code the game uploads with MPG and runs on `MSCAL`.

## Building and testing

```
cmake -S pc/gfx -B build/gfx -G Ninja
cmake --build build/gfx
ctest --test-dir build/gfx
```

`gfx_core_test` needs no GPU. `gfx_vulkan_test` draws through a Vulkan 1.3
device and reads the results back through GS transfers; Mesa's lavapipe is
enough. With SDL 3 available (`-DCMAKE_PREFIX_PATH=<SDL3 install>`),
`gfx_sdl_demo` opens a window and draws a moving scene.

In the WIN64 build, `pc/CMakeLists.txt` adds this directory and links it into
`chronicletwo_platform`; the Vulkan headers come from the Vulkan SDK or from
SDL's source tree.

Shaders are checked in as SPIR-V headers; after editing `shaders/`, build the
`gfx_shaders` target (needs `glslangValidator`).

## Not done yet

* Wiring the game: hardware registers it writes directly (DISPFB/DISPLAY,
  channel 8, timer 0) go through `pc/src/sce/pc_hardware.h`, and DMA tag
  addresses must be reachable through the `MemoryMap` (32-bit addresses).
* Registering the C++ VU1 microprograms with `Vu1Host`.
* GS features the renderer approximates: Ad as blend factor, Cs * (1 + C),
  mipmapping, AA1, dithering, PABE.
