#include "bank.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

namespace pc {
namespace audio {

namespace {

const uint32_t kChunkHeaderSize = 16;

// Fields of a program.
const uint32_t kProgramSplits = 0x00;
const uint32_t kProgramSplitCount = 0x04;
const uint32_t kProgramSplitSize = 0x05;
const uint32_t kProgramVolume = 0x06;
const uint32_t kProgramPan = 0x07;
const uint32_t kProgramTranspose = 0x08;
const uint32_t kProgramDetune = 0x09;

// Fields of a split.
const uint32_t kSplitSampleSet = 0x00;
const uint32_t kSplitLow = 0x02;
const uint32_t kSplitHigh = 0x04;
const uint32_t kSplitBendLow = 0x06;
const uint32_t kSplitBendHigh = 0x08;
const uint32_t kSplitVolume = 0x10;
const uint32_t kSplitPan = 0x11;
const uint32_t kSplitTranspose = 0x12;
const uint32_t kSplitDetune = 0x13;

// Fields of a sample set.
const uint32_t kSetVelocityLow = 0x01;
const uint32_t kSetVelocityHigh = 0x02;
const uint32_t kSetCount = 0x03;
const uint32_t kSetSamples = 0x04;

// Fields of a sample.
const uint32_t kSampleVag = 0x00;
const uint32_t kSampleVelocityLow = 0x02;
const uint32_t kSampleVelocityHigh = 0x04;
const uint32_t kSampleBaseNote = 0x0B;
const uint32_t kSampleDetune = 0x0C;
const uint32_t kSamplePan = 0x0D;
const uint32_t kSamplePriority = 0x0F;
const uint32_t kSampleVolume = 0x10;
const uint32_t kSampleAdsr1 = 0x12;
const uint32_t kSampleAdsr2 = 0x14;

// Fields of a VAG record.
const uint32_t kVagOffset = 0x00;
const uint32_t kVagRate = 0x04;

} // namespace

uint32_t Bank::U32(uint32_t offset) const {
    if (offset + 4 > header.size()) {
        return 0;
    }

    return header[offset] | (header[offset + 1] << 8) | (header[offset + 2] << 16) | ((uint32_t) header[offset + 3] << 24);
}

uint16_t Bank::U16(uint32_t offset) const {
    return offset + 2 <= header.size() ? (uint16_t) (header[offset] | (header[offset + 1] << 8)) : 0;
}

uint8_t Bank::U8(uint32_t offset) const {
    return offset < header.size() ? header[offset] : 0;
}

bool Bank::Load(const uint8_t *data, uint32_t size, uint32_t body_address) {
    header.assign(data, data + size);
    spu_address = body_address;
    programs = sample_sets = samples = vag_infos = 0;

    // The Head chunk, right after the Vers chunk, points at the others, as
    // modhsyn finds them; without one, walk the chunks: "IECS", the reversed
    // type, then the chunk's size.
    const uint32_t kHead = 0x10;

    if (size >= kHead + 0x24 && std::memcmp(&header[kHead], "IECSdaeH", 8) == 0) {
        const char    *types[] = {"gorP", "tesS", "lpmS", "igaV"};
        uint32_t      *chunks[] = {&programs, &sample_sets, &samples, &vag_infos};

        for (int i = 0; i < 4; i++) {
            uint32_t offset = U32(kHead + 0x14 + i * 4);

            if (offset != 0xFFFFFFFF && offset + kChunkHeaderSize <= size &&
                std::memcmp(&header[offset], "IECS", 4) == 0 && std::memcmp(&header[offset + 4], types[i], 4) == 0) {
                *chunks[i] = offset;
            }
        }
    }

    for (uint32_t offset = 0; offset + kChunkHeaderSize <= size;) {
        uint32_t length = U32(offset + 8);

        if (std::memcmp(&header[offset], "IECS", 4) != 0 || length < kChunkHeaderSize) {
            break;
        }

        const uint8_t *type = &header[offset + 4];

        if (std::memcmp(type, "gorP", 4) == 0 && programs == 0) {
            programs = offset;
        } else if (std::memcmp(type, "tesS", 4) == 0 && sample_sets == 0) {
            sample_sets = offset;
        } else if (std::memcmp(type, "lpmS", 4) == 0 && samples == 0) {
            samples = offset;
        } else if (std::memcmp(type, "igaV", 4) == 0 && vag_infos == 0) {
            vag_infos = offset;
        }

        offset += length;
    }

    if (programs == 0 || sample_sets == 0 || samples == 0 || vag_infos == 0) {
        SDL_Log("Sound bank header of %u bytes lacks a chunk (Prog %u, Sset %u, Smpl %u, Vagi %u)", size, programs,
                sample_sets, samples, vag_infos);
        header.clear();
        return false;
    }

    return true;
}

uint32_t Bank::Entry(uint32_t chunk, uint32_t index) const {
    if (chunk == 0 || index > U32(chunk + 12)) {
        return 0;
    }

    uint32_t offset = U32(chunk + kChunkHeaderSize + index * 4);
    return offset == 0xFFFFFFFF || chunk + offset >= header.size() ? 0 : chunk + offset;
}

int Bank::Find(int program, int note, int velocity, BankZone *zones, int max_zones) const {
    uint32_t prog = Entry(programs, program);
    int      found = 0;

    if (prog == 0) {
        return 0;
    }

    uint32_t splits = prog + U32(prog + kProgramSplits);
    int      split_count = U8(prog + kProgramSplitCount);
    int      split_size = U8(prog + kProgramSplitSize);

    for (int s = 0; s < split_count && found < max_zones; s++) {
        uint32_t split = splits + s * split_size;

        if (note < U8(split + kSplitLow) || note > U8(split + kSplitHigh)) {
            continue;
        }

        uint32_t set = Entry(sample_sets, U16(split + kSplitSampleSet));

        if (set == 0 || velocity < U8(set + kSetVelocityLow) || velocity > U8(set + kSetVelocityHigh)) {
            continue;
        }

        int count = U8(set + kSetCount);

        for (int i = 0; i < count && found < max_zones; i++) {
            uint32_t sample = Entry(samples, U16(set + kSetSamples + i * 2));

            if (sample == 0 || velocity < U8(sample + kSampleVelocityLow) || velocity > U8(sample + kSampleVelocityHigh)) {
                continue;
            }

            uint32_t vag = Entry(vag_infos, U16(sample + kSampleVag));

            if (vag == 0) {
                continue;
            }

            BankZone &zone = zones[found++];
            zone.spu_address = spu_address + U32(vag + kVagOffset);
            zone.sample_rate = U16(vag + kVagRate);
            zone.base_note = U8(sample + kSampleBaseNote);
            zone.detune = (int8_t) (U8(sample + kSampleDetune) + U8(split + kSplitDetune) + U8(prog + kProgramDetune));
            zone.transpose = (int8_t) (U8(split + kSplitTranspose) + U8(prog + kProgramTranspose));
            zone.volume = U8(sample + kSampleVolume) * U8(split + kSplitVolume) * U8(prog + kProgramVolume) / (127 * 127);
            zone.pan = (int8_t) U8(sample + kSamplePan) + (int8_t) U8(split + kSplitPan) + (int8_t) U8(prog + kProgramPan);
            zone.pan = std::min(std::max(zone.pan, -64), 63);
            zone.adsr1 = U16(sample + kSampleAdsr1);
            zone.adsr2 = U16(sample + kSampleAdsr2);
            zone.bend_range_low = U16(split + kSplitBendLow);
            zone.bend_range_high = U16(split + kSplitBendHigh);
            zone.priority = U8(sample + kSamplePriority);
        }
    }

    return found;
}

} // namespace audio
} // namespace pc
