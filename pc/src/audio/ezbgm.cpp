// EZBGM, the IOP server that streams audio files, on the PC. Channel n
// feeds core n's input, as the IOP module feeds it with block transfers, and
// the channel's volume is that input's volume (BVOL).
//
// The server reads RIFF WAVE files of 16-bit PCM, at any rate and in mono or
// stereo; anything else is taken as the core input's own format, 16-bit
// stereo in 512-byte blocks per side. Names are "cdrom0:\\..." paths on the
// disc, "host0:" files, or bare names, which are looked for anywhere on the
// disc.
//
// The command words come from the game's EE client (sound.cpp). What each
// reports back is inferred from how the game reads it:
//   0x8000 INIT      argument: buffer size (sent after OPEN)
//   0x0010 QUIT
//   0x8020 OPEN      argument: a 64-byte block holding the name; gives the format
//   0x80F0 OPEN_PACK argument: the pack's name and the member's (STREAM_PACK_REQUEST)
//   0x0030 CLOSE
//   0x0040 PRELOAD
//   0x0050 START
//   0x0060 STOP      (also pause)
//   0x0070 SEEK      back to the start
//   0x0080 SETVOL    argument: left << 16 | right
//   0x80B0 GETSTATE  0x1000 while playing, 0x8000 once the file has been read
//                    to the end, then 0 once its last sample has played
//   0x80C0 SETMODE   0x10 for mono
//   0x80D0 GETINFO   bit 0 set for stereo
//   0x80E0 GETLEVEL  the samples playing now, left << 16 | right

#include "../disc/disc.h"
#include "../host/host_paths.h"
#include "../iop/iop.h"
#include "servers.h"
#include "spu2.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace pc {
namespace audio {

namespace {

const int kChannels = 2;

enum BgmCommand {
    BGM_INIT = 0x8000,
    BGM_QUIT = 0x0010,
    BGM_OPEN = 0x8020,
    BGM_CLOSE = 0x0030,
    BGM_PRELOAD = 0x0040,
    BGM_START = 0x0050,
    BGM_STOP = 0x0060,
    BGM_SEEK = 0x0070,
    BGM_SET_VOLUME = 0x0080,
    BGM_GET_STATE = 0x80B0,
    BGM_SET_MODE = 0x80C0,
    BGM_GET_INFO = 0x80D0,
    BGM_GET_LEVEL = 0x80E0,
    BGM_OPEN_PACK = 0x80F0,
};

const int kStatePlaying = 0x1000;
const int kStateTerminated = 0x8000;
const int kInfoStereo = 0x0001;

/** How much the IOP module reads ahead of what plays, in frames: two buffers of the default size. */
const uint32_t kReadAheadFrames = 2 * 0x4000 / 4;

/** Mode bits set by SETMODE. */
const int kModeMono = 0x0010;

/** The bytes of a file, on the disc or the host. */
class Source {
public:
    bool Open(const std::string &name) {
        const char *colon = std::strchr(name.c_str(), ':');
        std::string device = colon != NULL ? name.substr(0, colon - name.c_str()) : std::string();
        std::string path = colon != NULL ? std::string(colon + 1) : name;

        if (device == "host0" || device == "host") {
            std::filesystem::path host;

            if (!ResolveNoCase(HostPath(), path, &host)) {
                return false;
            }

            stream.reset(new std::ifstream(host, std::ios::binary));
            std::error_code error;
            size = (uint64_t) std::filesystem::file_size(host, error);
            return *stream && !error;
        }

        if (!disc::Find(path, &file) && (colon != NULL || !disc::FindAnywhere(path, &file))) {
            return false;
        }

        size = file.size;
        return !file.directory;
    }

    /** Reads up to `count` bytes from `offset`; gives how many there were. */
    uint32_t Read(uint64_t offset, void *buffer, uint32_t count) {
        if (offset >= size) {
            return 0;
        }

        count = (uint32_t) std::min<uint64_t>(count, size - offset);

        if (stream != nullptr) {
            stream->clear();
            stream->seekg((std::streamoff) offset);
            stream->read(static_cast<char *>(buffer), count);
            return (uint32_t) stream->gcount();
        }

        return disc::Read((uint64_t) file.lsn * disc::kSectorSize + offset, buffer, count) ? count : 0;
    }

    uint64_t Size() const {
        return size;
    }

private:
    std::unique_ptr<std::ifstream> stream;
    disc::File                     file = {};
    uint64_t                       size = 0;
};

/** One stream channel, which plays through its core's input. */
class Channel : public CoreInput {
public:
    int core = 0;

    void Close() {
        source.reset();
        playing = false;
        state = 0;
    }

    /** Opens a file and reads its format; gives the GETINFO word, or 0 when it cannot be read. */
    int Open(const std::string &name) {
        Close();
        std::unique_ptr<Source> opened(new Source());

        if (!opened->Open(name)) {
            SDL_Log("EZBGM: cannot open %s", name.c_str());
            return 0;
        }

        source = std::move(opened);
        ReadFormat(name);
        Rewind();
        return info;
    }

    /** Opens a member of a pack. The pack format is not known yet, so this fails with a note. */
    int OpenPack(const std::string &pack, const std::string &member) {
        Close();
        SDL_Log("EZBGM: cannot open %s from pack %s: packs are not read yet", member.c_str(), pack.c_str());
        return 0;
    }

    void Rewind() {
        frame = 0;
        phase = 0;
        fetched = 0;
        state = playing ? kStatePlaying : 0;
    }

    void Start() {
        if (source != nullptr) {
            playing = true;

            if (state == 0) {
                state = kStatePlaying;
            }
        }
    }

    void Stop() {
        playing = false;
    }

    int State() const {
        return state | (mode & 0x0FFF);
    }

    int Level() const {
        return ((uint16_t) last_left << 16) | (uint16_t) last_right;
    }

    int info = 0;
    int mode = 0;

    void Next(int16_t *left, int16_t *right) override {
        *left = *right = 0;

        if (playing && source != nullptr) {
            Produce(left, right);
        }

        last_left = *left;
        last_right = *right;
    }

private:
    /** Reads the WAVE header, if there is one, to learn the layout of the samples. */
    void ReadFormat(const std::string &name) {
        uint8_t header[12];

        channels = 2;
        rate = kSampleRate;
        data_offset = 0;
        data_size = source->Size();
        interleaved = true;

        if (source->Read(0, header, 12) == 12 && std::memcmp(header, "RIFF", 4) == 0 &&
            std::memcmp(header + 8, "WAVE", 4) == 0) {
            uint64_t offset = 12;
            uint8_t  chunk[8];
            bool     pcm = false;

            while (source->Read(offset, chunk, 8) == 8) {
                uint32_t length = chunk[4] | (chunk[5] << 8) | (chunk[6] << 16) | ((uint32_t) chunk[7] << 24);

                if (std::memcmp(chunk, "fmt ", 4) == 0) {
                    uint8_t format[16] = {};
                    source->Read(offset + 8, format, 16);
                    pcm = (format[0] | (format[1] << 8)) == 1 && (format[14] | (format[15] << 8)) == 16;
                    channels = format[2] | (format[3] << 8);
                    rate = format[4] | (format[5] << 8) | (format[6] << 16) | ((uint32_t) format[7] << 24);
                } else if (std::memcmp(chunk, "data", 4) == 0) {
                    data_offset = offset + 8;
                    data_size = std::min<uint64_t>(length, source->Size() - data_offset);
                    break;
                }

                offset += 8 + length + (length & 1);
            }

            if (!pcm || channels < 1 || channels > 2 || rate == 0) {
                SDL_Log("EZBGM: %s is a WAVE file but not 16-bit PCM in mono or stereo", name.c_str());
                channels = 2;
                rate = kSampleRate;
            }

            interleaved = false;
        } else {
            SDL_Log("EZBGM: %s has no WAVE header; playing it as 16-bit stereo in 512-byte blocks", name.c_str());
        }

        info = channels == 2 ? kInfoStereo : 0;
        total_frames = data_size / (2 * channels);
    }

    /** The source frame `index`, or false past the end. */
    bool SourceFrame(uint64_t index, int16_t *left, int16_t *right) {
        if (index >= total_frames) {
            return false;
        }

        uint64_t offset;
        int16_t  samples[2] = {};

        if (interleaved) {
            // 256 left samples, then 256 right, per 1 KB block.
            uint64_t block = index / 256;
            uint64_t within = index % 256;
            offset = data_offset + block * 1024 + within * 2;
            Fetch(offset, &samples[0]);
            Fetch(offset + 512, &samples[1]);
        } else {
            offset = data_offset + index * 2 * channels;
            Fetch(offset, &samples[0]);

            if (channels == 2) {
                Fetch(offset + 2, &samples[1]);
            } else {
                samples[1] = samples[0];
            }
        }

        if (mode & kModeMono) {
            samples[1] = samples[0];
        }

        *left = samples[0];
        *right = samples[1];
        return true;
    }

    /** Reads one sample through a small cache of the file. */
    void Fetch(uint64_t offset, int16_t *sample) {
        if (offset < cache_offset || offset + 2 > cache_offset + fetched) {
            cache_offset = offset;
            fetched = source->Read(offset, cache, sizeof(cache));
        }

        if (offset + 2 <= cache_offset + fetched) {
            std::memcpy(sample, &cache[offset - cache_offset], 2);
        } else {
            *sample = 0;
        }
    }

    /** Gives the next 48 kHz frame, resampling linearly from the file's rate. */
    void Produce(int16_t *left, int16_t *right) {
        int16_t a_left, a_right, b_left, b_right;

        if (!SourceFrame(frame, &a_left, &a_right)) {
            playing = false;
            state = 0;
            return;
        }

        if (!SourceFrame(frame + 1, &b_left, &b_right)) {
            b_left = a_left;
            b_right = a_right;
        }

        *left = (int16_t) (a_left + ((b_left - a_left) * (int64_t) phase) / kSampleRate);
        *right = (int16_t) (a_right + ((b_right - a_right) * (int64_t) phase) / kSampleRate);

        phase += rate;

        while (phase >= (uint32_t) kSampleRate) {
            phase -= kSampleRate;
            frame++;
        }

        // The file has been read to its end once what is left fits in the read-ahead.
        uint64_t left_frames = total_frames - std::min(total_frames, frame);

        if (left_frames * kSampleRate / rate <= kReadAheadFrames) {
            state = kStateTerminated;
        }
    }

    std::unique_ptr<Source> source;
    bool                    playing = false;
    int                     state = 0;
    int                     channels = 2;
    uint32_t                rate = kSampleRate;
    uint64_t                data_offset = 0;
    uint64_t                data_size = 0;
    uint64_t                total_frames = 0;
    bool                    interleaved = true;
    uint64_t                frame = 0;
    uint32_t                phase = 0;
    uint8_t                 cache[0x4000];
    uint64_t                cache_offset = 0;
    uint32_t                fetched = 0;
    int16_t                 last_left = 0;
    int16_t                 last_right = 0;
};

Channel *GetChannels() {
    static Channel channels[kChannels];
    static bool    attached = false;

    if (!attached) {
        for (int i = 0; i < kChannels; i++) {
            channels[i].core = i;
            GetSpu().SetInput(i, &channels[i]);
        }

        attached = true;
    }

    return channels;
}

/** Copies a name out of a request block, which need not be terminated. */
std::string NameFrom(const void *data, int size, int offset, int length) {
    if (data == NULL || offset >= size) {
        return std::string();
    }

    const char *text = static_cast<const char *>(data) + offset;
    length = std::min(length, size - offset);
    return std::string(text, strnlen(text, length));
}

} // namespace

void *EzBgmServer(unsigned int function, void *data, int size) {
    static int reply[16];
    Channel   &channel = GetChannels()[function & 0x0F & (kChannels - 1)];
    int        argument = data != NULL && size >= 4 ? *static_cast<int *>(data) : 0;
    int        result = 0;

    StartOutput();

    switch (function & 0xFFF0) {
        case BGM_INIT:
            // Sent after OPEN, with the size of the buffers to read through; reads here are direct.
            break;
        case BGM_QUIT:
        case BGM_CLOSE:
            channel.Close();
            break;
        case BGM_OPEN:
            result = channel.Open(NameFrom(data, size, 0, 64));
            break;
        case BGM_OPEN_PACK:
            result = channel.OpenPack(NameFrom(data, size, 0, 52), NameFrom(data, size, 52, 12));
            break;
        case BGM_PRELOAD:
            break;
        case BGM_START:
            channel.Start();
            break;
        case BGM_STOP:
            channel.Stop();
            break;
        case BGM_SEEK:
            channel.Rewind();
            break;
        case BGM_SET_VOLUME:
            GetSpu().SetParam((uint16_t) (0x0F80 | channel.core), (uint16_t) (argument >> 16));
            GetSpu().SetParam((uint16_t) (0x1080 | channel.core), (uint16_t) argument);
            break;
        case BGM_GET_STATE:
            result = channel.State();
            break;
        case BGM_SET_MODE:
            channel.mode = argument;
            break;
        case BGM_GET_INFO:
            result = channel.info;
            break;
        case BGM_GET_LEVEL:
            result = channel.Level();
            break;
        default:
            SDL_Log("EZBGM: command 0x%X is not implemented", function);
            break;
    }

    std::memset(reply, 0, sizeof(reply));
    reply[0] = result;
    return reply;
}

} // namespace audio
} // namespace pc
