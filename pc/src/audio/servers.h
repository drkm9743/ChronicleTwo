#pragma once

// The game's sound modules on the IOP, run in-process: EZBGM streams audio
// files into a core's input, EZMIDI plays sequences and sound effects on the
// voices. Both are reached through SIF RPC (iop/sifrpc.cpp) and run under
// iop::Lock().

namespace pc {
namespace audio {

const unsigned int kEzBgmRpcNumber = 0x12345;
const unsigned int kEzMidiRpcNumber = 0x12346;

void *EzBgmServer(unsigned int function, void *data, int size);
void *EzMidiServer(unsigned int function, void *data, int size);

/** How often the IOP-side timers run, in ticks per second. */
const int kTickRate = 240;

/** Runs EZMIDI's timer: its sequencer and the MIDI stream input. */
void EzMidiTick();

/**
 * Starts the sound output the first time it is called: an SDL audio stream
 * that mixes the sound processor at 48 kHz and runs the IOP timers as it
 * goes. Without an audio device the mixing still runs, unheard, so the
 * servers keep time.
 */
void StartOutput();

/** Mixes `frames` frames and runs the timers they span, as the output does. */
void Render(short *out, int frames);

/**
 * Keeps StartOutput from starting anything, for a program that calls Render
 * itself (the tests); call it before any sound is played.
 */
void DisableOutput();

} // namespace audio
} // namespace pc
