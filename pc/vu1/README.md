# pc/vu1

Dark Cloud 2's five VU1 microprograms (PAL, SCES_511.90) translated to C++,
for the PC build. `pc/gfx/vu1/dc2_programs.cpp` hooks them into the emulated
VU1, and `pc/src/main` registers them with the packets the game uploads.

| Program | File | What it draws |
|---|---|---|
| Vu_progmain | `vu_prog_wtr.cpp` (`VuProgMainSetup`) | Resident kernel: per-object setup |
| Vu_prog0 | `vu_prog0*.cpp` | Models: skinning, lights, clipping |
| Vu_prog_sdw | `vu_prog_sdw.cpp` | Shadow volumes |
| Vu_prog_3dsp | `vu_prog_3dsp.cpp` | Camera-facing 3D sprites |
| Vu_prog_wtr | `vu_prog_wtr.cpp` | Water |

Each was checked word for word against the original microcode, run in a VU1
interpreter that follows PCSX2's rules, on random batches and on the DMA
chains of four PCSX2 save states (1.38 million GIF words, none different).
The interpreter, the tests and the disassembly are kept outside the
repository.

They compute as the VU does only with round-toward-zero, denormals flushed
to zero, no contraction and the rounding mode honoured: the adapter sets
MXCSR around each call, and CMake builds these files with
`-frounding-math -ffp-contract=off`.
