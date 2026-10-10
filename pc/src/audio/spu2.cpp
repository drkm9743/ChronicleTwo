#include "../iop/iop.h"
#include "spu2.h"

#include <algorithm>
#include <cstring>

namespace pc {
namespace audio {

namespace {

// Register numbers, bits 8-15 of a libsd entry.
enum Register {
    REG_VOLL = 0x00,
    REG_VOLR = 0x01,
    REG_PITCH = 0x02,
    REG_ADSR1 = 0x03,
    REG_ADSR2 = 0x04,
    REG_ENVX = 0x05,
    REG_VOLXL = 0x06,
    REG_VOLXR = 0x07,
    REG_MMIX = 0x08,
    REG_MVOLL = 0x09,
    REG_MVOLR = 0x0A,
    REG_EVOLL = 0x0B,
    REG_EVOLR = 0x0C,
    REG_AVOLL = 0x0D,
    REG_AVOLR = 0x0E,
    REG_BVOLL = 0x0F,
    REG_BVOLR = 0x10,
    REG_MVOLXL = 0x11,
    REG_MVOLXR = 0x12,
    REG_PMON = 0x13,
    REG_NON = 0x14,
    REG_KON = 0x15,
    REG_KOFF = 0x16,
    REG_ENDX = 0x17,
    REG_VMIXL = 0x18,
    REG_VMIXEL = 0x19,
    REG_VMIXR = 0x1A,
    REG_VMIXER = 0x1B,
    REG_ESA = 0x1C,
    REG_EEA = 0x1D,
    REG_TSA = 0x1E,
    REG_IRQA = 0x1F,
    REG_SSA = 0x20,
    REG_LSAX = 0x21,
    REG_NAX = 0x22,
};

// MMIX gates: which sources reach the dry output and the effect send.
enum MixGate {
    MMIX_EXT_WET_R = 0x001, /**< The previous core's output, to the effect. */
    MMIX_EXT_WET_L = 0x002,
    MMIX_EXT_DRY_R = 0x004, /**< The previous core's output, dry. */
    MMIX_EXT_DRY_L = 0x008,
    MMIX_IN_WET_R = 0x010, /**< The block input, to the effect. */
    MMIX_IN_WET_L = 0x020,
    MMIX_IN_DRY_R = 0x040, /**< The block input, dry. */
    MMIX_IN_DRY_L = 0x080,
    MMIX_VOICE_WET_R = 0x100, /**< The voices, to the effect. */
    MMIX_VOICE_WET_L = 0x200,
    MMIX_VOICE_DRY_R = 0x400, /**< The voices, dry. */
    MMIX_VOICE_DRY_L = 0x800,
};

/** ADPCM prediction filters. */
const int kFilters[5][2] = {
    {0,   0  },
    {60,  0  },
    {115, -52},
    {98,  -55},
    {122, -60},
};

/** Samples in one 16-byte ADPCM block. */
const int kBlockSamples = 28;

/** Samples per channel in one 1 KB block of a core input transfer. */
const int kInputBlockSamples = 256;

/** ADPCM block flags. */
const uint8_t kFlagEnd = 0x01;
const uint8_t kFlagRepeat = 0x02;
const uint8_t kFlagLoopStart = 0x04;

const uint32_t kVoiceMask = (1u << Spu2::kVoices) - 1;

int32_t Clamp16(int32_t value) {
    return std::min(std::max(value, -0x8000), 0x7FFF);
}

/** Cubic interpolation between `p1` and `p2`, `fraction` 12 bits of the way. */
int32_t Interpolate(const int16_t *p, uint32_t fraction) {
    float t = fraction / 4096.0f;
    float a = -0.5f * p[0] + 1.5f * p[1] - 1.5f * p[2] + 0.5f * p[3];
    float b = p[0] - 2.5f * p[1] + 2.0f * p[2] - 0.5f * p[3];
    float c = -0.5f * p[0] + 0.5f * p[2];
    return Clamp16((int32_t) (((a * t + b) * t + c) * t + p[1]));
}

} // namespace

Spu2 &GetSpu() {
    static Spu2 spu;
    return spu;
}

Spu2::Spu2() : ram(new uint8_t[kRamSize]) {
    std::memset(ram, 0, kRamSize);
    std::memset(cores, 0, sizeof(cores));
    Reset();
}

void Spu2::Reset() {
    for (int c = 0; c < kCores; c++) {
        Core      &core = cores[c];
        CoreInput *input = core.input;

        std::memset(&core, 0, sizeof(core));
        core.input = input;
        core.mmix = 0xFFC;
        core.master_left = core.master_right = 0x3FFF;
        core.master_level_left = core.master_level_right = 0x7FFE;
        core.ext_left = core.ext_right = 0x7FFF;
        core.input_left = core.input_right = 0x7FFF;
        core.vmixl = core.vmixr = core.vmixel = core.vmixer = kVoiceMask;
        core.eea = 0x1FFFFF - (c << 17);
        core.esa = core.eea;

        for (Voice &voice : core.voices) {
            voice.phase = PHASE_OFF;
        }
    }
}

int32_t Spu2::VolumeLevel(uint16_t value, int32_t current) {
    // Bit 15 clear: a fixed volume in bits 0-14, halved. Set: a sweep, which holds its level here.
    return (value & 0x8000) == 0 ? (int32_t) (int16_t) (value << 1) : current;
}

void Spu2::SetParam(uint16_t entry, uint16_t value) {
    Core  &core = cores[entry & 1];
    Voice &voice = core.voices[((entry >> 1) & 0x1F) % kVoices];

    switch (entry >> 8) {
        case REG_VOLL:
            voice.volume_left = value;
            voice.level_left = VolumeLevel(value, voice.level_left);
            break;
        case REG_VOLR:
            voice.volume_right = value;
            voice.level_right = VolumeLevel(value, voice.level_right);
            break;
        case REG_PITCH:
            voice.pitch = value;
            break;
        case REG_ADSR1:
            voice.adsr1 = value;
            break;
        case REG_ADSR2:
            voice.adsr2 = value;
            break;
        case REG_ENVX:
            voice.envelope = value & 0x7FFF;
            break;
        case REG_MMIX:
            core.mmix = value;
            break;
        case REG_MVOLL:
            core.master_left = value;
            core.master_level_left = VolumeLevel(value, core.master_level_left);
            break;
        case REG_MVOLR:
            core.master_right = value;
            core.master_level_right = VolumeLevel(value, core.master_level_right);
            break;
        case REG_EVOLL:
            core.effect_left = (int16_t) value;
            break;
        case REG_EVOLR:
            core.effect_right = (int16_t) value;
            break;
        case REG_AVOLL:
            core.ext_left = (int16_t) value;
            break;
        case REG_AVOLR:
            core.ext_right = (int16_t) value;
            break;
        case REG_BVOLL:
            core.input_left = (int16_t) value;
            break;
        case REG_BVOLR:
            core.input_right = (int16_t) value;
            break;
        default:
            break;
    }
}

uint16_t Spu2::GetParam(uint16_t entry) const {
    const Core  &core = cores[entry & 1];
    const Voice &voice = core.voices[((entry >> 1) & 0x1F) % kVoices];

    switch (entry >> 8) {
        case REG_VOLL:
            return voice.volume_left;
        case REG_VOLR:
            return voice.volume_right;
        case REG_PITCH:
            return voice.pitch;
        case REG_ADSR1:
            return voice.adsr1;
        case REG_ADSR2:
            return voice.adsr2;
        case REG_ENVX:
            return (uint16_t) voice.envelope;
        case REG_VOLXL:
            return (uint16_t) (voice.level_left >> 1);
        case REG_VOLXR:
            return (uint16_t) (voice.level_right >> 1);
        case REG_MMIX:
            return core.mmix;
        case REG_MVOLL:
            return core.master_left;
        case REG_MVOLR:
            return core.master_right;
        case REG_EVOLL:
            return (uint16_t) core.effect_left;
        case REG_EVOLR:
            return (uint16_t) core.effect_right;
        case REG_AVOLL:
            return (uint16_t) core.ext_left;
        case REG_AVOLR:
            return (uint16_t) core.ext_right;
        case REG_BVOLL:
            return (uint16_t) core.input_left;
        case REG_BVOLR:
            return (uint16_t) core.input_right;
        case REG_MVOLXL:
            return (uint16_t) (core.master_level_left >> 1);
        case REG_MVOLXR:
            return (uint16_t) (core.master_level_right >> 1);
        default:
            return 0;
    }
}

void Spu2::SetSwitch(uint16_t entry, uint32_t value) {
    Core &core = cores[entry & 1];
    value &= kVoiceMask;

    switch (entry >> 8) {
        case REG_PMON:
            core.pmon = value & ~1u;
            break;
        case REG_NON:
            core.non = value;
            break;
        case REG_KON:
            for (int v = 0; v < kVoices; v++) {
                if (value & (1u << v)) {
                    KeyOn(core, v);
                }
            }

            break;
        case REG_KOFF:
            for (int v = 0; v < kVoices; v++) {
                if (value & (1u << v)) {
                    KeyOff(core, v);
                }
            }

            break;
        case REG_ENDX:
            core.endx &= ~value;
            break;
        case REG_VMIXL:
            core.vmixl = value;
            break;
        case REG_VMIXEL:
            core.vmixel = value;
            break;
        case REG_VMIXR:
            core.vmixr = value;
            break;
        case REG_VMIXER:
            core.vmixer = value;
            break;
        default:
            break;
    }
}

uint32_t Spu2::GetSwitch(uint16_t entry) const {
    const Core &core = cores[entry & 1];

    switch (entry >> 8) {
        case REG_PMON:
            return core.pmon;
        case REG_NON:
            return core.non;
        case REG_ENDX:
            return core.endx;
        case REG_VMIXL:
            return core.vmixl;
        case REG_VMIXEL:
            return core.vmixel;
        case REG_VMIXR:
            return core.vmixr;
        case REG_VMIXER:
            return core.vmixer;
        default:
            return 0;
    }
}

void Spu2::SetAddr(uint16_t entry, uint32_t value) {
    Core  &core = cores[entry & 1];
    Voice &voice = core.voices[((entry >> 1) & 0x1F) % kVoices];

    value &= kRamSize - 1;

    switch (entry >> 8) {
        case REG_ESA:
            core.esa = value;
            break;
        case REG_EEA:
            core.eea = value;
            break;
        case REG_TSA:
            core.tsa = value;
            break;
        case REG_IRQA:
            core.irqa = value;
            break;
        case REG_SSA:
            voice.start = value & ~15u;
            break;
        case REG_LSAX:
            voice.loop_start = value & ~15u;
            break;
        case REG_NAX:
            voice.address = value & ~15u;
            break;
        default:
            break;
    }
}

uint32_t Spu2::GetAddr(uint16_t entry) const {
    const Core  &core = cores[entry & 1];
    const Voice &voice = core.voices[((entry >> 1) & 0x1F) % kVoices];

    switch (entry >> 8) {
        case REG_ESA:
            return core.esa;
        case REG_EEA:
            return core.eea;
        case REG_TSA:
            return core.tsa;
        case REG_IRQA:
            return core.irqa;
        case REG_SSA:
            return voice.start;
        case REG_LSAX:
            return voice.loop_start;
        case REG_NAX:
            return voice.address;
        default:
            return 0;
    }
}

void Spu2::SetCoreAttr(uint16_t entry, uint16_t value) {
    cores[entry & 1].attributes[(entry >> 1) & 7] = value;
}

uint16_t Spu2::GetCoreAttr(uint16_t entry) const {
    return cores[entry & 1].attributes[(entry >> 1) & 7];
}

void Spu2::SetEffect(int core, int mode, int16_t depth_left, int16_t depth_right, int delay, int feedback) {
    // Reverb is not emulated yet; the effect send is dropped.
    (void) core;
    (void) mode;
    (void) depth_left;
    (void) depth_right;
    (void) delay;
    (void) feedback;
}

void Spu2::WriteRam(uint32_t address, const void *data, uint32_t size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);

    for (uint32_t i = 0; i < size; i++) {
        ram[(address + i) & (kRamSize - 1)] = bytes[i];
    }
}

void Spu2::ReadRam(uint32_t address, void *data, uint32_t size) const {
    uint8_t *bytes = static_cast<uint8_t *>(data);

    for (uint32_t i = 0; i < size; i++) {
        bytes[i] = ram[(address + i) & (kRamSize - 1)];
    }
}

void Spu2::StartBlock(int core, uint32_t iop_address, uint32_t size, uint32_t start, bool loop) {
    BlockTransfer &block = cores[core & 1].block;

    block.base = iop_address;
    block.size = size & ~1023u;
    block.loop = loop;
    block.offset = start >= iop_address && start - iop_address < block.size ? (start - iop_address) & ~1023u : 0;
    block.sample = 0;
    block.active = block.size != 0;
}

uint32_t Spu2::StopBlock(int core) {
    BlockTransfer &block = cores[core & 1].block;
    block.active = false;
    return block.base + block.offset;
}

uint32_t Spu2::BlockStatus(int core) const {
    const BlockTransfer &block = cores[core & 1].block;
    uint32_t             half = block.offset >= block.size / 2 ? 1 : 0;
    return (half << 24) | ((block.base + block.offset) & 0xFFFFFF);
}

void Spu2::SetInput(int core, CoreInput *input) {
    cores[core & 1].input = input;
}

void Spu2::KeyOn(Core &core, int index) {
    Voice &voice = core.voices[index];

    voice.address = voice.start;
    voice.counter = 0;
    voice.adpcm_history[0] = voice.adpcm_history[1] = 0;
    std::memset(voice.recent, 0, sizeof(voice.recent));
    voice.phase = PHASE_ATTACK;
    voice.envelope = 0;
    voice.envelope_counter = 0;
    voice.output = 0;
    core.endx &= ~(1u << index);
    DecodeBlock(voice);
}

void Spu2::KeyOff(Core &core, int index) {
    Voice &voice = core.voices[index];

    if (voice.phase != PHASE_OFF) {
        voice.phase = PHASE_RELEASE;
        voice.envelope_counter = 0;
    }
}

void Spu2::DecodeBlock(Voice &voice) {
    uint32_t       address = voice.address & (kRamSize - 1) & ~15u;
    const uint8_t *block = &ram[address];
    int            shift = block[0] & 0x0F;
    int            filter = std::min(block[0] >> 4, 4);

    voice.block_flags = block[1];

    if (voice.block_flags & kFlagLoopStart) {
        voice.loop_start = address;
    }

    if (shift > 12) {
        shift = 9;
    }

    for (int i = 0; i < kBlockSamples; i++) {
        int     nibble = (block[2 + i / 2] >> ((i & 1) * 4)) & 0x0F;
        int32_t sample = (int32_t) (int16_t) (nibble << 12) >> shift;

        sample += (voice.adpcm_history[0] * kFilters[filter][0] + voice.adpcm_history[1] * kFilters[filter][1] + 32) >> 6;
        sample = Clamp16(sample);
        voice.adpcm_history[1] = voice.adpcm_history[0];
        voice.adpcm_history[0] = (int16_t) sample;
        voice.block[i] = (int16_t) sample;
    }

    voice.block_index = 0;
}

int16_t Spu2::NextSample(Core &core, int index) {
    Voice &voice = core.voices[index];

    if (voice.block_index >= kBlockSamples) {
        if (voice.block_flags & kFlagEnd) {
            core.endx |= 1u << index;

            if (voice.block_flags & kFlagRepeat) {
                voice.address = voice.loop_start;
            } else {
                // The end without a repeat: the voice falls silent at once.
                voice.phase = PHASE_OFF;
                voice.envelope = 0;
                voice.address = voice.loop_start;
            }
        } else {
            voice.address = (voice.address + 16) & (kRamSize - 1);
        }

        DecodeBlock(voice);
    }

    return voice.block[voice.block_index++];
}

void Spu2::StepEnvelope(Voice &voice) {
    int  rate;
    bool decreasing;
    bool exponential;

    switch (voice.phase) {
        case PHASE_ATTACK:
            rate = (voice.adsr1 >> 8) & 0x7F;
            decreasing = false;
            exponential = (voice.adsr1 & 0x8000) != 0;
            break;
        case PHASE_DECAY:
            rate = ((voice.adsr1 >> 4) & 0x0F) << 2;
            decreasing = true;
            exponential = true;
            break;
        case PHASE_SUSTAIN:
            rate = (voice.adsr2 >> 6) & 0x7F;
            decreasing = (voice.adsr2 & 0x4000) != 0;
            exponential = (voice.adsr2 & 0x8000) != 0;
            break;
        case PHASE_RELEASE:
            rate = (voice.adsr2 & 0x1F) << 2;
            decreasing = true;
            exponential = (voice.adsr2 & 0x0020) != 0;
            break;
        default:
            return;
    }

    int     shift = rate >> 2;
    int32_t step = decreasing ? -8 + (rate & 3) : 7 - (rate & 3);
    int32_t cycles = 1 << std::max(0, shift - 11);

    step *= 1 << std::max(0, 11 - shift);

    if (exponential && !decreasing && voice.envelope > 0x6000) {
        cycles *= 4;
    }

    if (exponential && decreasing) {
        step = (step * voice.envelope) >> 15;
    }

    if (++voice.envelope_counter < cycles) {
        return;
    }

    voice.envelope_counter = 0;
    voice.envelope = std::min(std::max(voice.envelope + step, 0), 0x7FFF);

    switch (voice.phase) {
        case PHASE_ATTACK:
            if (voice.envelope >= 0x7FFF) {
                voice.phase = PHASE_DECAY;
            }

            break;
        case PHASE_DECAY:
            if (voice.envelope <= std::min(((voice.adsr1 & 0x0F) + 1) * 0x800, 0x7FFF)) {
                voice.phase = PHASE_SUSTAIN;
            }

            break;
        case PHASE_RELEASE:
            if (voice.envelope == 0) {
                voice.phase = PHASE_OFF;
            }

            break;
        default:
            break;
    }
}

int32_t Spu2::VoiceSample(Core &core, int index) {
    Voice &voice = core.voices[index];

    if (voice.phase == PHASE_OFF) {
        voice.output = 0;
        return 0;
    }

    int32_t sample = (Interpolate(voice.recent, voice.counter & 0xFFF) * voice.envelope) >> 15;
    voice.output = sample;

    uint32_t step = std::min<uint32_t>(voice.pitch, 0x3FFF);

    if (index > 0 && (core.pmon & (1u << index))) {
        step = (uint32_t) (((int64_t) step * (core.voices[index - 1].output + 0x8000)) >> 15);
        step = std::min<uint32_t>(step, 0x3FFF);
    }

    voice.counter = (voice.counter & 0xFFF) + step;

    for (uint32_t advance = voice.counter >> 12; advance > 0 && voice.phase != PHASE_OFF; advance--) {
        int16_t next = NextSample(core, index);
        std::memmove(&voice.recent[0], &voice.recent[1], 3 * sizeof(int16_t));
        voice.recent[3] = next;
    }

    StepEnvelope(voice);
    return sample;
}

void Spu2::ReadInput(Core &core, int32_t *left, int32_t *right) {
    int16_t        l = 0;
    int16_t        r = 0;
    BlockTransfer &block = core.block;

    if (block.active) {
        const int16_t *samples = static_cast<const int16_t *>(iop::Translate(block.base + block.offset, 1024));

        if (samples == NULL) {
            block.active = false;
        } else {
            l = samples[block.sample];
            r = samples[kInputBlockSamples + block.sample];

            if (++block.sample == kInputBlockSamples) {
                block.sample = 0;
                block.offset += 1024;

                if (block.offset >= block.size) {
                    block.offset = 0;
                    block.active = block.loop;
                }
            }
        }
    } else if (core.input != NULL) {
        core.input->Next(&l, &r);
    }

    *left = (l * core.input_left) >> 15;
    *right = (r * core.input_right) >> 15;
}

void Spu2::Mix(int16_t *out, int frames) {
    for (int frame = 0; frame < frames; frame++) {
        int32_t previous_left = 0;
        int32_t previous_right = 0;

        for (int c = 0; c < kCores; c++) {
            Core   &core = cores[c];
            int32_t voices_left = 0;
            int32_t voices_right = 0;

            for (int v = 0; v < kVoices; v++) {
                int32_t sample = VoiceSample(core, v);

                if (sample == 0) {
                    continue;
                }

                if (core.vmixl & (1u << v)) {
                    voices_left += (sample * core.voices[v].level_left) >> 15;
                }

                if (core.vmixr & (1u << v)) {
                    voices_right += (sample * core.voices[v].level_right) >> 15;
                }
            }

            int32_t input_left;
            int32_t input_right;
            ReadInput(core, &input_left, &input_right);

            int32_t ext_left = c > 0 ? (previous_left * core.ext_left) >> 15 : 0;
            int32_t ext_right = c > 0 ? (previous_right * core.ext_right) >> 15 : 0;
            int32_t left = 0;
            int32_t right = 0;

            if (core.mmix & MMIX_VOICE_DRY_L) {
                left += voices_left;
            }

            if (core.mmix & MMIX_VOICE_DRY_R) {
                right += voices_right;
            }

            if (core.mmix & MMIX_IN_DRY_L) {
                left += input_left;
            }

            if (core.mmix & MMIX_IN_DRY_R) {
                right += input_right;
            }

            if (core.mmix & MMIX_EXT_DRY_L) {
                left += ext_left;
            }

            if (core.mmix & MMIX_EXT_DRY_R) {
                right += ext_right;
            }

            previous_left = Clamp16((Clamp16(left) * core.master_level_left) >> 15);
            previous_right = Clamp16((Clamp16(right) * core.master_level_right) >> 15);
        }

        out[frame * 2] = (int16_t) previous_left;
        out[frame * 2 + 1] = (int16_t) previous_right;
    }
}

} // namespace audio
} // namespace pc
