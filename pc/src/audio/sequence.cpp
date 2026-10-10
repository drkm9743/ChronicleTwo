#include "../iop/iop.h"
#include "sequence.h"

#include <SDL3/SDL.h>

#include <cstring>

namespace pc {
namespace audio {

namespace {

const uint32_t kChunkHeaderSize = 16;

/** NRPN values of CC 99 that mark a loop. */
const int kLoopStart = 20;
const int kLoopEnd = 30;

const int kMaxEventsPerAdvance = 100000;

uint32_t Le32(const uint8_t *bytes) {
    return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

} // namespace

const uint8_t *Sequence::Data() const {
    return static_cast<const uint8_t *>(iop::Translate(address, size));
}

bool Sequence::Load(uint32_t iop_address) {
    loaded = false;
    playing = false;

    if (iop_address < iop::kMemoryBase || iop_address >= iop::kMemoryBase + iop::kMemorySize) {
        return false;
    }

    address = iop_address;
    size = iop::kMemoryBase + iop::kMemorySize - iop_address;
    const uint8_t *data = Data();

    // Find the Midi chunk, then its first song.
    for (uint32_t offset = 0; offset + kChunkHeaderSize <= size;) {
        uint32_t length = Le32(data + offset + 8);

        if (std::memcmp(data + offset, "IECS", 4) != 0 || length < kChunkHeaderSize) {
            break;
        }

        if (std::memcmp(data + offset + 4, "idiM", 4) == 0) {
            uint32_t song = offset + Le32(data + offset + kChunkHeaderSize);

            if (song + 6 > size) {
                break;
            }

            start = song + Le32(data + song);
            division = data[song + 4] | (data[song + 5] << 8);
            size = std::min(size, offset + length);
            loaded = start < size && division != 0;
            break;
        }

        offset += length;
    }

    if (!loaded) {
        SDL_Log("EZMIDI: no sequence at IOP address 0x%X", iop_address);
        return false;
    }

    Rewind();
    return true;
}

void Sequence::Rewind() {
    position = start;
    tempo = 500000;
    running = 0;
    pending = 0;
    wait = 0;
    waiting = false;
    loop_set = false;
}

uint32_t Sequence::ReadVarLen() {
    const uint8_t *data = Data();
    uint32_t       value = 0;

    while (position < size) {
        uint8_t byte = data[position++];
        value = (value << 7) | (byte & 0x7F);

        if (!(byte & 0x80)) {
            break;
        }
    }

    return value;
}

void Sequence::Advance(double microseconds, Synth &synth, int port) {
    if (!playing) {
        return;
    }

    pending += microseconds * division / tempo;

    // A loop with no time in it would never let go; give up on it.
    for (int events = 0; events < kMaxEventsPerAdvance; events++) {
        const uint8_t *data = Data();

        if (data == NULL || position >= size) {
            playing = false;
            return;
        }

        if (!waiting) {
            wait = ReadVarLen();
            waiting = true;
        }

        if (pending < wait) {
            return;
        }

        pending -= wait;
        waiting = false;

        uint8_t status = data[position];

        if (status & 0x80) {
            position++;
        } else {
            status = running;
        }

        if (status == 0xFF) {
            uint8_t  type = position < size ? data[position++] : 0;
            uint32_t length = ReadVarLen();

            if (type == 0x2F) {
                // The end of the song: play on from the loop's start if one was marked.
                if (loop_set) {
                    position = loop_position;
                    tempo = loop_tempo;
                    running = loop_running;
                    continue;
                }

                playing = false;
                return;
            }

            if (type == 0x51 && length == 3 && position + 3 <= size) {
                uint32_t value = (data[position] << 16) | (data[position + 1] << 8) | data[position + 2];

                // Ticks still to play were counted at the old tempo.
                pending = pending * tempo / (value != 0 ? value : tempo);
                tempo = value != 0 ? value : tempo;
            }

            position += length;
            continue;
        }

        if (status == 0xF0 || status == 0xF7) {
            position += ReadVarLen();
            continue;
        }

        if (!(status & 0x80)) {
            // Data with no status before it: the sequence is not what it was taken for.
            playing = false;
            return;
        }

        running = status;
        uint8_t message[3] = {status, 0, 0};
        int     length = (status & 0xE0) == 0xC0 ? 1 : 2;

        for (int i = 0; i < length && position < size; i++) {
            message[1 + i] = data[position++];
        }

        if ((status & 0xF0) == 0xB0 && message[1] == 99) {
            if (message[2] == kLoopStart) {
                loop_position = position;
                loop_tempo = tempo;
                loop_running = running;
                loop_set = true;
            } else if (message[2] == kLoopEnd && loop_set) {
                position = loop_position;
                tempo = loop_tempo;
                running = loop_running;
            }

            continue;
        }

        synth.Message(port, message);
    }

    SDL_Log("EZMIDI: the sequence on port %d loops without advancing; stopping it", port);
    playing = false;
}

} // namespace audio
} // namespace pc
