// EZMIDI, the IOP server that plays the game's music and sound effects, on
// the PC: 16 ports, each with sound banks (synth.h), a sequence (sequence.h)
// and, for ports 7-15, a MIDI stream input buffer the game fills with sound
// effect messages (msin.cpp) and copies to IOP memory once a frame.
//
// The command words are the ones MODULES/EZMIDI.IRX on the disc answers;
// the port is in the low four bits. That module runs Sony's modhsyn and
// modmidi with a 240 Hz tick:
//   0x8010 INIT      argument: size of the stream input buffers; gives their IOP address
//   0x80F0 QUIT
//   0x0000 PLAY      plays the port's sequence from where it is
//   0x0020 STOP      stops it (here its voices are released too)
//   0x0030 REWIND    argument: where in the sequence to go; the game only sends 0
//   0x0040 SEQUENCE  argument: IOP address of a .SQ
//   0x9050 BANK      argument: a block holding the bank's number, its .HD's IOP
//                    address and the sound memory address of its .BD
//   0x00A0 PRIORITY  argument: the port's priority for voices, in the low byte
//   0x00B0 VOLUME    argument: 0-256, 256 full
//   0x00C0 STEREO    argument: the synthesizer's output mode, 1 stereo and 0
//                    mono (modhsyn keeps every voice centred for 0)
// The module also answers status, volume and voice-count queries and BD
// transfers (0x80D0, 0x80E0, 0x8090, 0x9060-0x9080, 0x7000, 0x8100), which
// the game never sends.

#include "../iop/iop.h"
#include "sequence.h"
#include "servers.h"
#include "synth.h"

#include <SDL3/SDL.h>

#include <cstring>

namespace pc {
namespace audio {

namespace {

const int kPorts = 16;
const int kStreamFirstPort = 7;
const int kStreamPorts = 9;

/** One stream input buffer as the game lays it out (MSIN_BUFFER). */
const uint32_t kStreamBufferSize = 0x200;
const uint32_t kStreamHeaderSize = 8;

enum MidiCommand {
    MIDI_PLAY = 0x000,
    MIDI_INIT = 0x010,
    MIDI_STOP = 0x020,
    MIDI_REWIND = 0x030,
    MIDI_SEQUENCE = 0x040,
    MIDI_BANK = 0x050,
    MIDI_PRIORITY = 0x0A0,
    MIDI_VOLUME = 0x0B0,
    MIDI_STEREO = 0x0C0,
    MIDI_QUIT = 0x0F0,
};

/** The block BANK sends (MIDI_BANK), as the game's compiler lays it out. */
struct BankRequest {
    int32_t bank_no;
    void   *hd_address;
    void   *bd_address;
    int32_t bd_size;
    int32_t spu_address;
};

struct State {
    Synth    synth;
    Sequence sequences[kPorts];
    uint32_t stream_address = 0;
    uint32_t stream_size = 0;
};

State &GetState() {
    static State state;
    return state;
}

uint32_t Le32(const uint8_t *bytes) {
    return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

/** How long the .HD at an IOP address is, from its Head chunk. */
uint32_t HeaderSize(uint32_t address) {
    uint32_t       available = iop::kMemoryBase + iop::kMemorySize - address;
    const uint8_t *data = static_cast<const uint8_t *>(iop::Translate(address, available));
    uint32_t       offset = 0;

    if (data == NULL) {
        return 0;
    }

    while (offset + 16 <= available && std::memcmp(data + offset, "IECS", 4) == 0) {
        uint32_t length = Le32(data + offset + 8);

        if (std::memcmp(data + offset + 4, "daeH", 4) == 0) {
            return std::min(Le32(data + offset + 12), available);
        }

        if (length < 16) {
            break;
        }

        offset += length;
    }

    return offset;
}

/** Bytes of a message that starts with `status`, or 0 for a byte that starts none. */
int MessageLength(uint8_t status) {
    if (status == 0xFD) {
        return 7;
    }

    if (status == 0xF9) {
        return 5;
    }

    if (status >= 0x80 && status < 0xF0) {
        return (status & 0xE0) == 0xC0 ? 2 : 3;
    }

    return 0;
}

/** Plays what the game has put in the stream input buffers since the last tick. */
void ReadStreams(State &state) {
    for (int i = 0; i < kStreamPorts; i++) {
        uint32_t address = state.stream_address + i * kStreamBufferSize;

        if (address + kStreamBufferSize > state.stream_address + state.stream_size) {
            break;
        }

        uint8_t *buffer = static_cast<uint8_t *>(iop::Translate(address, kStreamBufferSize));
        int32_t  length;

        std::memcpy(&length, buffer + 4, 4);
        length = std::min<int32_t>(length, kStreamBufferSize - kStreamHeaderSize);

        const uint8_t *messages = buffer + kStreamHeaderSize;
        int            port = kStreamFirstPort + i;

        for (int offset = 0; offset < length;) {
            int size = MessageLength(messages[offset]);

            if (size == 0 || offset + size > length) {
                offset++;
                continue;
            }

            if (messages[offset] >= 0xF0) {
                state.synth.EffectMessage(port, messages + offset);
            } else {
                state.synth.Message(port, messages + offset);
            }

            offset += size;
        }

        std::memset(buffer + 4, 0, 4);
    }
}

} // namespace

void EzMidiTick() {
    State &state = GetState();

    if (state.stream_address != 0) {
        ReadStreams(state);
    }

    for (int port = 0; port < kPorts; port++) {
        state.sequences[port].Advance(1000000.0 / kTickRate, state.synth, port);
    }

    state.synth.Update();
}

void *EzMidiServer(unsigned int function, void *data, int size) {
    static int reply[16];
    State     &state = GetState();
    int        port = function & 0x0F;
    int        argument = data != NULL && size >= 4 ? *static_cast<int *>(data) : 0;
    int        result = 0;

    StartOutput();

    switch (function & 0x0FF0) {
        case MIDI_INIT:
            if (state.stream_address == 0) {
                state.stream_size = (uint32_t) argument;
                state.stream_address = iop::Alloc(state.stream_size);

                if (void *buffers = iop::Translate(state.stream_address, state.stream_size)) {
                    std::memset(buffers, 0, state.stream_size);
                }
            }

            result = (int) state.stream_address;
            break;
        case MIDI_QUIT:
            for (int p = 0; p < kPorts; p++) {
                state.sequences[p].Unload();
                state.synth.ResetPort(p);
            }

            iop::Free(state.stream_address);
            state.stream_address = 0;
            break;
        case MIDI_PLAY:
            state.sequences[port].Play();
            break;
        case MIDI_STOP:
            state.sequences[port].Stop();
            state.synth.ResetPort(port);
            break;
        case MIDI_REWIND:
            state.sequences[port].Rewind();
            break;
        case MIDI_SEQUENCE:
            state.sequences[port].Load((uint32_t) argument);
            break;
        case MIDI_BANK: {
            BankRequest request = {};
            std::memcpy(&request, data, std::min<size_t>(sizeof(request), size));
            uint32_t       hd = iop::AddressOf(request.hd_address);
            uint32_t       hd_size = HeaderSize(hd);
            const uint8_t *header = static_cast<const uint8_t *>(iop::Translate(hd, hd_size));

            if (header == NULL || hd_size == 0) {
                SDL_Log("EZMIDI: no sound bank header at IOP address 0x%X", hd);
                result = -1;
                break;
            }

            state.synth.SetBank(port, request.bank_no, header, hd_size, (uint32_t) request.spu_address);
            break;
        }
        case MIDI_PRIORITY:
            state.synth.SetPortPriority(port, argument & 0xFF);
            break;
        case MIDI_VOLUME:
            state.synth.SetPortVolume(port, argument);
            break;
        case MIDI_STEREO:
            state.synth.SetMono(argument == 0);
            break;
        default:
            SDL_Log("EZMIDI: command 0x%X is not implemented", function);
            break;
    }

    std::memset(reply, 0, sizeof(reply));
    reply[0] = result;
    return reply;
}

} // namespace audio
} // namespace pc
