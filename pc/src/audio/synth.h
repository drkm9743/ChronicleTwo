#pragma once

// The hardware synthesizer (modhsyn) on the PC: MIDI messages on 16 ports,
// each with its own sound banks, played on the 48 voices of the SPU2.
// Voices are shared by all ports; when none is free, the quietest claim
// loses one: a released voice before a held one, a lower-priority port's
// before a higher one's, the oldest first.
//
// Each port also takes the sound-effect messages of the MIDI stream input:
//   F9 00 00 vv 00        volume of the effects that follow (0-127)
//   F9 01 00 pp 00        their pan (0-127, 64 centre)
//   F9 02 00 ll hh        their pitch (14 bits)
//   FD 10 00 kk ii vv 00  starts effect `ii` on key `kk` at velocity `vv`; 0 stops it
//   FD 00 00 kk ii vv 00  sets a playing effect's volume
//   FD 01 00 kk ii pp 00  sets its pan
//   FD 02 00 kk ii ll hh  sets its pitch
// Effects play the program and bank chosen on MIDI channel 0.

#include "bank.h"

#include <cstdint>

namespace pc {
namespace audio {

class Synth {
public:
    static const int kPorts = 16;
    static const int kBanksPerPort = 16;

    Synth();

    /** Makes `hd` (`size` bytes, its body at `spu_address`) bank `bank_no` of a port. */
    void SetBank(int port, int bank_no, const uint8_t *hd, uint32_t size, uint32_t spu_address);

    /** Silences a port and puts its controllers back. */
    void ResetPort(int port);

    /** Sets a port's volume: 256 is full. */
    void SetPortVolume(int port, int volume);

    /** Sets a port's priority for voices: higher keeps them. */
    void SetPortPriority(int port, int priority);

    /** Mixes everything to the middle when true. */
    void SetMono(bool mono);

    /** Plays one MIDI channel message (status byte first). */
    void Message(int port, const uint8_t *message);

    /** Plays one sound-effect message (F9 or FD first). */
    void EffectMessage(int port, const uint8_t *message);

    /** Frees the voices that have fallen silent; run once a tick. */
    void Update();

private:
    static const int kVoices = 48;
    static const int kChannels = 16;
    static const int kZonesPerNote = 4;

    struct Channel {
        int  program;
        int  bank;
        int  volume;     /**< CC 7. */
        int  expression; /**< CC 11. */
        int  pan;        /**< CC 10, 64 centre. */
        int  bend;       /**< 14 bits, 8192 centre. */
        bool sustain;    /**< CC 64. */
    };

    struct Port {
        Bank    banks[kBanksPerPort];
        Channel channels[kChannels];
        int     volume;
        int     priority;
        int     effect_volume;
        int     effect_pan;
        int     effect_pitch;
    };

    struct Voice {
        bool     used;
        bool     released;
        bool     sustained; /**< Released while the sustain pedal was down. */
        int      port;
        int      channel;
        int      note;
        int      effect_id; /**< The effect's id, or -1 for a MIDI note. */
        int      velocity;
        int      volume; /**< An effect's own volume, 0-127. */
        int      pan;    /**< An effect's own pan, 64 centre. */
        int      bend;   /**< An effect's own pitch, 8192 centre. */
        uint32_t age;
        BankZone zone;
    };

    void NoteOn(int port, int channel, int note, int velocity, int effect_id);
    void NoteOff(int port, int channel, int note, int effect_id);
    void Release(int index);
    int  Allocate(int port);
    void Apply(int index, bool pitch, bool volume);
    void ResetChannel(Channel &channel);

    Port     ports[kPorts];
    Voice    voices[kVoices];
    uint32_t clock;
    bool     mono;
};

} // namespace audio
} // namespace pc
