#pragma once

// Sony's sound bank headers (.HD), read from IOP memory: which sample in the
// bank body (.BD, already in sound memory) plays for a program, a key and a
// velocity, and how.
//
// An HD is a run of chunks, each starting "IECS" and a reversed four-letter
// type: Vers, Head, Vagi (where each sample's ADPCM data lies in the body),
// Smpl (how each sample plays), Sset (sets of samples split by velocity) and
// Prog (programs, split by key into sample sets).

#include <cstdint>
#include <vector>

namespace pc {
namespace audio {

/** One sample that plays for a key: everything a voice needs to start it. */
struct BankZone {
    uint32_t spu_address; /**< Where its ADPCM data starts in sound memory. */
    uint32_t sample_rate;
    uint8_t  base_note;
    int8_t   detune;    /**< In 1/128 semitone, sample and split and program together. */
    int8_t   transpose; /**< In semitones, split and program together. */
    int      volume;    /**< 0 to 127, sample, split and program together. */
    int      pan;       /**< -64 (left) to 63 (right), sample, split and program together. */
    uint16_t adsr1, adsr2;
    uint16_t bend_range_low, bend_range_high; /**< Pitch bend range, in semitones. */
    uint8_t  priority;
};

class Bank {
public:
    /** Reads a header from `size` bytes at `header`; the body is at `spu_address` in sound memory. */
    bool Load(const uint8_t *header, uint32_t size, uint32_t spu_address);

    bool Loaded() const {
        return !header.empty();
    }

    /** Finds what plays for a key and velocity on a program; gives how many zones were written. */
    int Find(int program, int note, int velocity, BankZone *zones, int max_zones) const;

private:
    uint32_t U32(uint32_t offset) const;
    uint16_t U16(uint32_t offset) const;
    uint8_t  U8(uint32_t offset) const;

    /** The offset of entry `index` of a chunk with an offset table, or 0 when there is none. */
    uint32_t Entry(uint32_t chunk, uint32_t index) const;

    std::vector<uint8_t> header;
    uint32_t             spu_address = 0;
    uint32_t             programs = 0;
    uint32_t             sample_sets = 0;
    uint32_t             samples = 0;
    uint32_t             vag_infos = 0;
};

} // namespace audio
} // namespace pc
