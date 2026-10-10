#pragma once

// A software SPU2, the PS2's sound processor: two cores of 24 voices playing
// ADPCM from 2 MB of sound memory, each core with an input fed by block
// transfers (streams and movie sound), mixed as the hardware mixes them, at
// 48 kHz. Registers are addressed the way libsd addresses them, so libsdr
// and the IOP servers drive it as they drove the hardware.
//
// Not yet emulated: reverb (the effect send is dropped), noise, volume
// sweeps (a sweeping volume holds its level) and IRQs.
//
// All of it runs under iop::Lock().

#include <cstdint>

namespace pc {
namespace audio {

const int kSampleRate = 48000;

/** Supplies a core's input when no block transfer is feeding it. */
class CoreInput {
public:
    virtual ~CoreInput() {}

    /** Gives the next stereo frame. */
    virtual void Next(int16_t *left, int16_t *right) = 0;
};

class Spu2 {
public:
    static const uint32_t kRamSize = 2 * 1024 * 1024;
    static const int      kCores = 2;
    static const int      kVoices = 24;

    Spu2();

    /** Puts every register back as sceSdInit leaves it, and stops all input. */
    void Reset();

    // Registers, by the entry numbers of libsd: the core in bit 0, the voice
    // in bits 1-5, the register in bits 8-15.
    void     SetParam(uint16_t entry, uint16_t value);
    uint16_t GetParam(uint16_t entry) const;
    void     SetSwitch(uint16_t entry, uint32_t value);
    uint32_t GetSwitch(uint16_t entry) const;
    void     SetAddr(uint16_t entry, uint32_t value);
    uint32_t GetAddr(uint16_t entry) const;
    void     SetCoreAttr(uint16_t entry, uint16_t value);
    uint16_t GetCoreAttr(uint16_t entry) const;
    void     SetEffect(int core, int mode, int16_t depth_left, int16_t depth_right, int delay, int feedback);

    void WriteRam(uint32_t address, const void *data, uint32_t size);
    void ReadRam(uint32_t address, void *data, uint32_t size) const;

    /**
     * Feeds a core's input from IOP memory, as an ADMA block transfer does:
     * 512 bytes of left samples, then 512 of right, over and over, from
     * `start` on, wrapping at the end of the buffer when `loop` is set.
     */
    void StartBlock(int core, uint32_t iop_address, uint32_t size, uint32_t start, bool loop);

    /** Stops a core's block transfer and gives the IOP address it had reached. */
    uint32_t StopBlock(int core);

    /** The IOP address a core's block transfer has reached, with the half of the buffer in bit 24. */
    uint32_t BlockStatus(int core) const;

    /** Sets what feeds a core's input when no block transfer does; NULL for silence. */
    void SetInput(int core, CoreInput *input);

    /** Whether a voice is sounding: keyed on and not yet silent. */
    bool VoiceActive(int core, int voice) const {
        return cores[core & 1].voices[voice % kVoices].phase != PHASE_OFF;
    }

    /** Mixes `frames` stereo frames into `out`. */
    void Mix(int16_t *out, int frames);

private:
    enum EnvelopePhase {
        PHASE_OFF,
        PHASE_ATTACK,
        PHASE_DECAY,
        PHASE_SUSTAIN,
        PHASE_RELEASE,
    };

    struct Voice {
        uint16_t      volume_left, volume_right; /**< Registers as written. */
        int32_t       level_left, level_right;   /**< Current volumes, -0x8000 to 0x7FFF. */
        uint16_t      pitch;
        uint16_t      adsr1, adsr2;
        uint32_t      start;      /**< SSA: where key-on starts, in bytes. */
        uint32_t      loop_start; /**< LSAX, in bytes. */
        uint32_t      address;    /**< NAX: the block being played, in bytes. */
        EnvelopePhase phase;
        int32_t       envelope;         /**< ENVX, 0 to 0x7FFF. */
        int32_t       envelope_counter; /**< Samples since the envelope last stepped. */
        int16_t       block[28];        /**< The decoded block being played. */
        uint8_t       block_flags;
        int           block_index; /**< Next sample of `block` to take. */
        int16_t       adpcm_history[2];
        int16_t       recent[4]; /**< The last four samples taken, oldest first, to interpolate. */
        uint32_t      counter;   /**< Pitch counter: 12 bits of fraction. */
        int32_t       output;    /**< Last sample out, before volume; drives pitch modulation. */
    };

    struct BlockTransfer {
        bool     active;
        bool     loop;
        uint32_t base;   /**< IOP address of the buffer. */
        uint32_t size;   /**< Bytes in the buffer. */
        uint32_t offset; /**< Byte of the buffer the current 1 KB block starts at. */
        int      sample; /**< Sample within the block, 0 to 255. */
    };

    struct Core {
        Voice         voices[kVoices];
        uint16_t      mmix;
        uint16_t      master_left, master_right; /**< MVOL registers as written. */
        int32_t       master_level_left, master_level_right;
        int16_t       effect_left, effect_right; /**< EVOL. */
        int16_t       ext_left, ext_right;       /**< AVOL: the previous core's output. */
        int16_t       input_left, input_right;   /**< BVOL: the block input. */
        uint32_t      pmon, non, vmixl, vmixel, vmixr, vmixer, endx;
        uint32_t      esa, eea, tsa, irqa;
        uint16_t      attributes[8];
        BlockTransfer block;
        CoreInput    *input;
    };

    void    KeyOn(Core &core, int voice);
    void    KeyOff(Core &core, int voice);
    void    DecodeBlock(Voice &voice);
    int16_t NextSample(Core &core, int index);
    int32_t VoiceSample(Core &core, int index);
    void    StepEnvelope(Voice &voice);
    void    ReadInput(Core &core, int32_t *left, int32_t *right);

    static int32_t VolumeLevel(uint16_t value, int32_t current);

    uint8_t *ram;
    Core     cores[kCores];
};

/** The one sound processor; lock iop::Lock() to use it. */
Spu2 &GetSpu();

} // namespace audio
} // namespace pc
