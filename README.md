# Chronicle Two

[![PAL Progress]](https://decomp.dev/TheMoonPeople/ChronicleTwo/pal)

[PAL Progress]: https://decomp.dev/TheMoonPeople/ChronicleTwo/pal.svg?mode=shield&label=PAL&measure=matched_code_percent
[progress_link]: https://decomp.dev/TheMoonPeople/ChronicleTwo/

[<img src="https://decomp.dev/TheMoonPeople/ChronicleTwo/pal.svg?w=512&h=256" width="512" height="256" alt="PAL decompilation progress">][progress_link]

Chronicle Two is a decompilation project (and eventual port) of Dark
Chronicle/Dark Cloud 2 for the PlayStation 2.

# Building and running

1. Clone the repository with `git clone --recurse-submodules https://github.com/TheMoonPeople/ChronicleTwo.git`
2. Place the PAL retail build named `Dark Chronicle (PAL).iso` in the `rom/pal/` folder at the root of the project.
3. Build [Satan's Fiddle](https://github.com/Adubbz/SatansFiddle) with its documented LLDB and unstripped `wibo` prerequisites. Set `SATANSFIDDLE` to the resulting executable, or place `satansfiddle` on `PATH`.
4. Run `build.sh`.

`build.sh` builds the game.
`run.sh` builds the disc image and boots it in PCSX2.

MWCC 3.0 compilation runs through Satan's Fiddle using
[the JSON profile](scripts/build/satansfiddle.json). `SATANSFIDDLE_CONFIG` selects
a different profile. CMake also accepts `-DSATANSFIDDLE=/absolute/path/satansfiddle`
and `-DSATANSFIDDLE_CONFIG=/absolute/path/profile.json`; these cached settings
take precedence over later environment changes. MWLD still runs under plain `wibo`.
See [the compiler integration notes](scripts/build/SATANSFIDDLE.md) for the wrapper,
selectors, and validation commands.

# Building for Windows (in progress)

`PLATFORM=WIN64` compiles the same game sources for 64-bit Windows with Clang
(clang-cl, or clang++ targeting `x86_64-pc-windows-msvc`); MSVC's own `cl` does
not accept them. It needs no disc image:

```
cmake -S . -B build/win64 -G Ninja -DPLATFORM=WIN64 -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build/win64
```

So far this builds every game unit into a static library, and a platform
library that implements the PS2 SDK on [SDL 3](https://libsdl.org) 3.4.18.
Only libpad is there yet: any controller SDL supports reads as a DualShock 2,
with rumble. The rest of the SDK and the Metrowerks runtime are still missing,
so nothing links the game yet. CMake uses an installed SDL 3.4.18 when it
finds one, and otherwise downloads and builds that release. The sources see `PLATFORM_PC` and `NONMATCHING`,
which MWCC never defines: PC-only code sits behind `#ifdef PLATFORM_PC`, and
the C versions of the VU0 assembly behind `#ifdef NONMATCHING`, so the PS2
build's objects stay byte-identical. `-DPC_LAYOUT_ASSERTS=ON` checks the PS2
structure sizes as well; with 8-byte pointers many of them differ.
