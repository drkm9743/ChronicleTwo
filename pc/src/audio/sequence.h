#pragma once

// A sequence player (modmidi) for Sony's .SQ files: a chunked file like the
// sound bank header ("IECS" and reversed types) whose Midi chunk holds the
// song as Standard MIDI File track events. Loops are marked the Sony way,
// with NRPN: CC 99 = 20 marks the start, CC 99 = 30 jumps back to it.
//
// The data stays in IOP memory, where the game put it; the player keeps its
// address and reads it from there.

#include "synth.h"

#include <cstdint>

namespace pc {
namespace audio {

class Sequence {
public:
    /** Takes the sequence at an IOP address; false when it is not a sequence. */
    bool Load(uint32_t iop_address);

    void Unload() {
        loaded = false;
        playing = false;
    }

    /** Goes back to the start. */
    void Rewind();

    void Play() {
        playing = loaded;
    }

    void Stop() {
        playing = false;
    }

    bool Playing() const {
        return playing;
    }

    /** Plays what falls due in the next `microseconds`, on `port` of `synth`. */
    void Advance(double microseconds, Synth &synth, int port);

private:
    const uint8_t *Data() const;
    uint32_t       ReadVarLen();

    bool     loaded = false;
    bool     playing = false;
    uint32_t address = 0;     /**< IOP address of the sequence. */
    uint32_t size = 0;        /**< Bytes of it that can be read. */
    uint32_t start = 0;       /**< Offset of the first event. */
    uint32_t position = 0;    /**< Offset of the next event. */
    uint32_t division = 480;  /**< Ticks per quarter note. */
    uint32_t tempo = 500000;  /**< Microseconds per quarter note. */
    uint8_t  running = 0;     /**< Running status. */
    double   pending = 0;     /**< Ticks elapsed and not yet played. */
    uint32_t wait = 0;        /**< Ticks until the next event. */
    bool     waiting = false; /**< Whether `wait` has been read for the next event. */
    uint32_t loop_position = 0;
    uint32_t loop_tempo = 500000;
    uint8_t  loop_running = 0;
    bool     loop_set = false;
};

} // namespace audio
} // namespace pc
