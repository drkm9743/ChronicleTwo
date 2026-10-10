# PS2 SDK on the PC: disc, memory card, SIF and sound

These folders implement the SDK calls the game makes for its disc, its memory
cards and its sound, for the WIN64 build (`chronicletwo_platform`). The game's
own code is unchanged: it calls the same functions, and these answer as the
PS2 did.

| Folder | SDK calls | What it does |
|---|---|---|
| `host/` | | Where the disc, `host0:` and the memory cards are; `sdk.h` includes the SDK headers from host code |
| `iop/` | `sceSif*` (RPC, IOP heap, SIF DMA) | IOP memory as a 4 MB block with small addresses; the RPC servers run in-process |
| `disc/` | `sceCd*`, `sceOpen` and the rest of `sifdev.h` | The disc from an ISO image or the extracted folder; `host0:` from a folder |
| `memcard/` | `sceMc*` | Each card is a folder: `slot1`, and `slot2` when it exists |
| `audio/` | `sceSdRemote*`, `sceMSIn_*`, EZBGM and EZMIDI | A software SPU2 mixed through SDL 3, and the game's IOP sound modules |

## Where things are

| | Environment variable | Default |
|---|---|---|
| Disc (ISO image or folder) | `DC2_DISC` | `disc` beside the executable |
| `host0:` | `DC2_HOST` | `host` beside the executable |
| Memory cards | `DC2_MEMCARD` | `memcard` in SDL's preference folder (`%APPDATA%\ChronicleTwo\Dark Chronicle` on Windows) |

`pc::SetDiscPath` and its siblings in `host/host_paths.h` override them.

Names on the disc match in either case, with or without `;1`. A folder disc
gives each file a run of sector numbers the first time the game looks it up,
so reading DATA.DAT and the movies by sector works as it does on an image.

## Sound

The IOP modules the game loads (LIBSD, SDRDRV, MODMIDI, MODHSYN, EZMIDI,
EZBGM) are replaced by:

- `spu2.cpp`: two cores of 24 ADPCM voices with ADSR, pitch modulation and
  the core inputs (block transfers), mixed as MMIX routes them, at 48 kHz.
  Not emulated yet: reverb, noise, volume sweeps.
- `libsdr.cpp`: `sceSdRemote`, the libsd calls on the SPU2.
- `ezbgm.cpp`: streams audio files into a core's input.
- `ezmidi.cpp`, `synth.cpp`, `bank.cpp`, `sequence.cpp`: the music and sound
  effects, from .HD/.BD banks and .SQ sequences.
- `msin.cpp`: the EE side of the MIDI stream input.
- `output.cpp`: the SDL 3 audio stream, which also runs the IOP timers.

The command words of EZBGM and EZMIDI, and the layout of the .HD and .SQ
files, are read from the game's EE code and from Sony's published formats,
not from the IOP modules themselves; each file's header comment lists what is
inferred. They are the first thing to check against the real modules and
data: the stream file formats (.WV, the .VPK packs), the pan convention of
the banks, the loop markers of the sequences.

## Tests

`pc/tests` builds these sources on the host with their own disc, memory card
and sound data; see its CMakeLists.txt.
