# SOUND.DAT, SOUND.HD3 and the EZBGM stream format

What WP5.3 asks for: the layout of `SOUND.HD3`, what `SOUND.DAT` holds, and
how `EZBGM.IRX` finds and plays an entry. Sources: the PAL disc files
(`SOUND.HD3` 86,848 bytes, `SOUND.DAT` 969,644,032 bytes, `DATA.HD3`
354,912 bytes) parsed with a throwaway script, and `MODULES/EZBGM.IRX`
(it keeps its symbols) read with the container's MIPS objdump. Nothing is
copied from the module. `(unchecked)` marks a statement that the disc files
do not settle, because no `SOUND.DAT` entry was read.

**The short answer.** `SOUND.HD3` is a flat array of 16-byte records
followed by the name strings. `SOUND.DAT` is the files laid end to end on
sector boundaries. Every entry is a `.wav`: a RIFF WAVE file of 16-bit PCM
that the module streams straight into an SPU2 core input. There are no VAG
blocks, no ADPCM and no stream header of the module's own. The section 3.8
expectation ("interleaved VAG blocks with a header") does not hold, so
WP5.4 needs a PCM WAVE channel, not a streamed-VAG player.

---

## 1. SOUND.HD3

Little-endian. `N = word[0] / 16` records, then the names.

| Offset | Size | Field |
|---|---|---|
| 0x0 | 4 | Byte offset of the name from the start of the file |
| 0x4 | 4 | File size in bytes |
| 0x8 | 4 | First sector, relative to the start of `SOUND.DAT` (2048-byte sectors) |
| 0xC | 4 | Sector count, always `ceil(size / 2048)` |

- There is no count field. The module takes the count from the first record's
  name offset: `InitCDFile` (0x2D68) stores `word[0] >> 4` in `header_num`,
  so the names start right after the last record.
- The last record is a terminator: name offset 86,844 (an empty string),
  size, sector and count 0. Real entries: **3,101**.
- The names are NUL-terminated, 12 bytes apart (`"0010010.wav"` plus NUL),
  in record order. They are flat: no directories, all lower case, all
  `NNNNNNN.wav`.
- Records are in sector order with no gaps: the last entry ends exactly at
  the size of `SOUND.DAT`.
- One name appears twice: `8640040.wav` at sectors 473,291 and 473,375, both
  170,322 bytes. The lookup takes the first one.
- Sizes range from 31,100 to 1,648,298 bytes.

`InitCDFile` loads the whole file into a 0x1E000-byte buffer
(`header_buff`), adds the buffer address to every name offset, and turns
`\` into `/` in the names (none of them contain one). It runs on `SDINIT`
(0x00D0) and, lazily, on the first open of a bare name.

**`DATA.HD3`** has the same layout (8,753 records, 8,742 non-empty, the
last ending exactly at the size of `DATA.DAT`). The game reads `DATA.HD4`
instead (`dataread.cpp:520`), so `DATA.HD3` is a leftover. The 354,912 bytes
that WP5.3 lists belong to `DATA.HD3`, not to `SOUND.HD3`.

## 2. Finding an entry

`strFileOpen` (0x25BC) sorts names into three cases:

| Name sent | Opened as |
|---|---|
| `cdrom0:\...` | A disc file found with `sceCdSearchFile` (upper-cased, `;1` added), streamed with `sceCdSt*` |
| any other `dev:...` | `open`/`lseek`/`read` through ioman (`host0:` in development) |
| no colon | A member of `SOUND.DAT`: `SearchFile` (0x2528) runs a linear, case-sensitive `strcmp` over the records; the first match wins. Streaming starts at `SOUND.DAT`'s LSN plus the record's sector |

The game sends bare names, built as `"%07d.wav"` (`event_func.cpp:14613`),
`"85200%d.wav"`/`"8520%d.wav"` (`dng_object.cpp:1972-1974`),
`"8500%s.wav"` (`inventmn.cpp:3521`), and the table in `menucapt.cpp:90`.
It also sends `cdrom0:\VOICE\...` paths (`event_func.cpp:13729`); those
directories are not on the PAL disc, so the open fails as it did on retail.

A port can extract each record to `data/sound/<name>` and open it by name.
To match retail, the duplicate must resolve to the first record.

## 3. Entry format

`BgmOpen` (0x09CC) reads the start of the file and checks, in order:

1. The bytes at 0 are `RIFF`; otherwise it prints `This is not RIFF!!` and
   OPEN answers **-1**.
2. The bytes at 8 are `WAVE`; otherwise **-1** too.
3. It walks the chunks to `fmt `. From the format block it reads only
   `nChannels` (the u16 at +2): 2 means stereo, anything else mono.
   **`wFormatTag`, `nSamplesPerSec` and `wBitsPerSample` are not read.**
4. It walks on to `data` and keeps its offset and size.

The samples are then sent with `sceSdBlockTrans` into the core's input:
`BgmRaw2Spu` (0x04B0) splits interleaved stereo into the 512-byte
left/right blocks the input expects, and `_BgmRaw2SpuMono` (0x0270)
duplicates a mono stream into both. The core input plays 16-bit PCM at
48 kHz, so retail plays every entry as 48 kHz s16le whatever its header says.
Whether all 3,101 headers say 48 kHz and 16-bit is **(unchecked)**. A quick
check is to read the `fmt ` chunk of each extracted entry.

OPEN answers `(data size & ~0xF) | stereo`, with bit 0 set for stereo, and
0 when the file cannot be opened. `GETINFO` (0x80D0) answers the same value
later.

`OPEN_FROM_PACK` (0x80F0) goes to `BgmOpenFromFPL` (0x1458). It only prints
`FPL WAVE FORMAT NOT SUPORT NOW!!!` and answers 0, so `.VPK` voice packs
never play on retail. Their paths are not on the PAL disc either. Note
that `CSound::StreamOpenFromFPLFast` (`sound.cpp:936-941`) writes the
pack's path into `STREAM_PACK_REQUEST::name` and the `.wav` name into
`pack_name`. The field names in `sound.hpp:133` are the other way round
from how the game uses them.

## 4. EZBGM commands

RPC number 0x12345. The channel is in the low four bits (0 and 1, one per
SPU2 core); `bgmFunc` (0x3004) switches on the rest.

| Command | Function | Argument, answer |
|---|---|---|
| 0x8000 | `BgmInit` | buffer size |
| 0x0010 | `BgmQuit` | |
| 0x8020 | `BgmOpen` | address of a 64-byte name; answers as in section 3 |
| 0x8A00 | open, then init, set mode (0x10 for mono) and preload | 64-byte name |
| 0x80F0 | `BgmOpenFromFPL` | `STREAM_PACK_REQUEST`; answers 0 |
| 0x0030 | `BgmClose` | |
| 0x0040 | `BgmPreLoad` | |
| 0x0050 | `BgmStart` | plays at the volume 0x0080 last set |
| 0x0060 | `BgmStop` | sets the input volume (BVOL) to 0; a later START continues |
| 0x0070 | `BgmSeek` | sector from the start of the file |
| 0x0080 | `BgmSetVolume` | `left << 16 \| right`, applied on START |
| 0x0090 | `BgmSetVolumeDirect` | the same, applied at once |
| 0x00A0 | `BgmSetMasterVolume` | the core's MVOL, `left << 16 \| right` |
| 0x00D0 | `BgmSdInit` | loads `SOUND.HD3` |
| 0x80B0 | `BgmGetMode` | the mode, with 0x1000 while playing and 0x8000 once the file has been read to the end; neither once the last sample has played or the channel was stopped |
| 0x80C0 | `BgmSetMode` | low eight bits of the mode; 0x10 = mono |
| 0x80D0 | `BgmGetInfo` | what OPEN answered |
| 0x80E0 | `BgmGetNowLevel` | the samples playing now, `left << 16 \| right` |

The game's sequences: open sends 0x80 (volume 0) then 0x8020
(`sound.cpp:931-932`); close sends 0x60, 0x30, 0x10; end sends 0x60, 0x70,
0x10 (`sound.cpp:953-961`).

## 5. What this changes in the plan

- **WP5.3:** the index is section 1. `tools/dc2data` can write
  `data/sound/<name>` straight from the records, with no conversion; the
  extracted files are ordinary WAVE files.
- **WP5.4:** the shared stream channel plays 16-bit PCM WAVE (mono or
  stereo, 48 kHz on retail) with START/STOP/SEEK in sectors, the two volume
  commands, the mode bits above and the level reading. It needs no VAG
  decoder. Packs (`0x80F0`) answer 0, as retail does.
- **Section 3.8 and risk "SOUND.HD3 format":** both can be closed.
