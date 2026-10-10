#pragma once

// The game's disc, read from an ISO image or from a folder holding the
// extracted disc (see host/host_paths.h for where it is looked for).
//
// The game finds files by name and then reads them by sector: DATA.DAT is
// one large file whose members it reads at sector offsets from its start,
// and the movies are streamed by sector. An ISO image has real sector
// numbers. For a folder, each file is given a run of sectors of its own the
// first time it is looked up, with a gap after it, so a read by sector finds
// its way back to the file.

#include <cstdint>
#include <string>

namespace pc {
namespace disc {

const uint32_t kSectorSize = 2048;

struct File {
    uint32_t    lsn;       /**< First sector. */
    uint32_t    size;      /**< Size in bytes. */
    std::string name;      /**< Name of the last path component, as the disc spells it. */
    bool        directory; /**< Whether this is a directory. */
};

/** Opens the disc, the first time only. False when there is no disc where it is looked for. */
bool Mount();

/** Forgets the disc so the next Mount opens it again, from wherever DiscPath now points. */
void Unmount();

/**
 * Looks a path up on the disc. Components are split by '\\' or '/', a
 * leading separator and a ";1" version suffix are optional, and letters
 * match in either case.
 */
bool Find(const std::string &path, File *file);

/** Looks for a file by its name alone, anywhere on the disc. */
bool FindAnywhere(const std::string &name, File *file);

/**
 * Reads `size` bytes from byte `offset` of the disc. What lies beyond a file
 * (in the gap a folder disc leaves) reads as zeros. False when the disc
 * cannot be read there.
 */
bool Read(uint64_t offset, void *buffer, uint32_t size);

/** Reads whole sectors. */
inline bool ReadSectors(uint32_t lsn, uint32_t count, void *buffer) {
    return Read((uint64_t) lsn * kSectorSize, buffer, count * kSectorSize);
}

} // namespace disc
} // namespace pc
