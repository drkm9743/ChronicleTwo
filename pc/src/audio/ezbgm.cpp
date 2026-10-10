// EZBGM, the IOP server that streams audio files, on the PC. Channel n
// feeds core n's input, as the IOP module feeds it with block transfers, and
// the channel's volume is that input's volume (BVOL).
//
// What follows is taken from the module on the disc (MODULES/EZBGM.IRX,
// version 1.2.1). It plays RIFF WAVE files of 16-bit PCM in mono or stereo;
// here they may be at any rate. A name is found as the module finds it:
//   "cdrom0:\\..."  a file on the disc (letters made upper case, ";1" added)
//   "host0:..."     a file in the host folder
//   a bare name     a member of SOUND.DAT, listed in SOUND.HD3 as 16-byte
//                   entries {name offset, size, sector, sectors}; the first
//                   name offset over 16 is how many entries there are
//
// Commands, the channel in the low four bits:
//   0x8000 INIT        argument: buffer size; has nothing to do here
//   0x0010 QUIT
//   0x8020 OPEN        argument: a 64-byte block holding the name; gives the size of
//                      the samples rounded down to 16, with bit 0 set for stereo; 0
//                      when the file cannot be opened, -1 when it is not RIFF WAVE
//   0x8A00 OPEN_LOAD   OPEN, then INIT, SETMODE (0x10 for mono) and PRELOAD
//   0x80F0 OPEN_PACK   argument: the pack's name and the member's (STREAM_PACK_REQUEST)
//   0x0030 CLOSE
//   0x0040 PRELOAD
//   0x0050 START       plays at the volume SETVOL last set
//   0x0060 STOP        silences the channel; START goes on from where it was
//   0x0070 SEEK        argument: sector from the start of the file
//   0x0080 SETVOL      argument: left << 16 | right, kept for START
//   0x0090 SETVOL_NOW  the same, set at once
//   0x00A0 MASTERVOL   the core's master volume (MVOL), left << 16 | right
//   0x00D0 SDINIT      reads SOUND.HD3
//   0x80B0 GETMODE     the mode SETMODE set, with 0x1000 while playing, 0x8000 once
//                      the file has been read to the end, and neither once its last
//                      sample has played or the channel was stopped
//   0x80C0 SETMODE     argument: the low eight bits of the mode, 0x10 for mono
//   0x80D0 GETINFO     what OPEN gave
//   0x80E0 GETLEVEL    the samples playing now, left << 16 | right

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
    BGM_OPEN_LOAD = 0x8A00,
    BGM_OPEN_PACK = 0x80F0,
    BGM_CLOSE = 0x0030,
    BGM_PRELOAD = 0x0040,
    BGM_START = 0x0050,
    BGM_STOP = 0x0060,
    BGM_SEEK = 0x0070,
    BGM_SET_VOLUME = 0x0080,
    BGM_SET_VOLUME_NOW = 0x0090,
    BGM_SET_MASTER_VOLUME = 0x00A0,
    BGM_SD_INIT = 0x00D0,
    BGM_GET_MODE = 0x80B0,
    BGM_SET_MODE = 0x80C0,
    BGM_GET_INFO = 0x80D0,
    BGM_GET_LEVEL = 0x80E0,
};

const int kStatePlaying = 0x1000;
const int kStateTerminated = 0x8000;
const int kInfoStereo = 0x0001;

/** Mode bits set by SETMODE. */
const int kModeMono = 0x0010;

/** How much the IOP module reads ahead of what plays, in frames: two buffers of the default size. */
const uint32_t kReadAheadFrames = 2 * 0x4000 / 4;

// libsd entries of a core's volumes (the core is or'd in).
const uint16_t kMasterVolumeLeft = 0x0980;
const uint16_t kMasterVolumeRight = 0x0A80;
const uint16_t kInputVolumeLeft = 0x0F80;
const uint16_t kInputVolumeRight = 0x1080;

uint32_t Le32(const uint8_t *bytes) {
    return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

/** The members of SOUND.DAT, as SOUND.HD3 lists them. */
class SoundTable {
public:
    struct Entry {
        std::string name;
        uint32_t    size;
        uint32_t    sector;
    };

    /** Reads SOUND.HD3 the first time; false when the disc has no SOUND.DAT. */
    bool Load() {
        if (loaded) {
            return !entries.empty();
        }

        loaded = true;
        disc::File header;

        if (!disc::Find("SOUND.HD3", &header) || !disc::Find("SOUND.DAT", &data)) {
            SDL_Log("EZBGM: the disc has no SOUND.HD3 and SOUND.DAT");
            return false;
        }

        std::vector<uint8_t> bytes(header.size);

        if (bytes.size() < 16 || !disc::Read((uint64_t) header.lsn * disc::kSectorSize, bytes.data(), header.size)) {
            SDL_Log("EZBGM: cannot read SOUND.HD3");
            return false;
        }

        uint32_t count = std::min<uint32_t>(Le32(&bytes[0]) / 16, (uint32_t) bytes.size() / 16);

        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *entry = &bytes[i * 16];
            uint32_t       name = Le32(entry);

            if (name >= bytes.size()) {
                continue;
            }

            const char *text = reinterpret_cast<const char *>(&bytes[name]);
            entries.push_back({std::string(text, strnlen(text, bytes.size() - name)), Le32(entry + 4), Le32(entry + 8)});
        }

        return !entries.empty();
    }

    /** Finds a member by name: exactly, as the module does, or else in either case. */
    const Entry *Find(const std::string &name) {
        if (!Load()) {
            return NULL;
        }

        for (const Entry &entry : entries) {
            if (entry.name == name) {
                return &entry;
            }
        }

        for (const Entry &entry : entries) {
            if (NamesEqualNoCase(entry.name, name)) {
                return &entry;
            }
        }

        return NULL;
    }

    /** The disc byte where a member starts. */
    uint64_t Offset(const Entry &entry) const {
        return ((uint64_t) data.lsn + entry.sector) * disc::kSectorSize;
    }

private:
    bool               loaded = false;
    disc::File         data = {};
    std::vector<Entry> entries;
};

SoundTable &GetSoundTable() {
    static SoundTable table;
    return table;
}

/** The bytes of a file, on the disc or the host. */
class Source {
public:
    bool Open(const std::string &name) {
        const char *colon = std::strchr(name.c_str(), ':');

        if (colon == NULL) {
            SoundTable              &table = GetSoundTable();
            const SoundTable::Entry *entry = table.Find(name);

            if (entry == NULL) {
                return false;
            }

            base = table.Offset(*entry);
            size = entry->size;
            return true;
        }

        std::string device = name.substr(0, colon - name.c_str());
        std::string path = colon + 1;

        if (device == "cdrom0" || device == "cdrom") {
            disc::File file;

            if (!disc::Find(path, &file) || file.directory) {
                return false;
            }

            base = (uint64_t) file.lsn * disc::kSectorSize;
            size = file.size;
            return true;
        }

        std::filesystem::path host;

        if (!ResolveNoCase(HostPath(), path, &host)) {
            return false;
        }

        stream.reset(new std::ifstream(host, std::ios::binary));
        std::error_code error;
        size = (uint64_t) std::filesystem::file_size(host, error);
        return *stream && !error;
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

        return disc::Read(base + offset, buffer, count) ? count : 0;
    }

    uint64_t Size() const {
        return size;
    }

private:
    std::unique_ptr<std::ifstream> stream;
    uint64_t                       base = 0;
    uint64_t                       size = 0;
};

/** One stream channel, which plays through its core's input. */
class Channel : public CoreInput {
public:
    int core = 0;
    int info = 0;
    int mode = 0;

    void Close() {
        Stop();
        source.reset();
        info = 0;
    }

    /** Opens a file and reads its format; gives what OPEN answers. */
    int Open(const std::string &name) {
        Close();
        std::unique_ptr<Source> opened(new Source());

        if (!opened->Open(name)) {
            SDL_Log("EZBGM: cannot open %s", name.c_str());
            return 0;
        }

        source = std::move(opened);

        if (!ReadFormat(name)) {
            source.reset();
            return -1;
        }

        Seek(0);
        info = (int) (data_size & ~(uint64_t) 0xF) | (channels == 2 ? kInfoStereo : 0);
        return info;
    }

    /** Opens a member of a pack. The pack format is not known yet, so this fails with a note. */
    int OpenPack(const std::string &pack, const std::string &member) {
        Close();
        SDL_Log("EZBGM: cannot open %s from pack %s: packs are not read yet", member.c_str(), pack.c_str());
        return 0;
    }

    /** Goes to a sector of the file, which for 0 is the start of its samples. */
    void Seek(int sector) {
        uint64_t offset = (uint64_t) std::max(sector, 0) * disc::kSectorSize;

        frame = offset > data_offset ? (offset - data_offset) / (2 * channels) : 0;
        phase = 0;
        fetched = 0;
    }

    void Start() {
        if (source != nullptr) {
            playing = true;
            state = kStatePlaying;
            ApplyVolume(volume);
        }
    }

    void Stop() {
        playing = false;
        state = 0;
        ApplyVolume(0);
    }

    void SetVolume(int value) {
        volume = value;

        if (playing) {
            ApplyVolume(volume);
        }
    }

    void ApplyVolume(int value) {
        GetSpu().SetParam((uint16_t) (kInputVolumeLeft | core), (uint16_t) (value >> 16));
        GetSpu().SetParam((uint16_t) (kInputVolumeRight | core), (uint16_t) value);
    }

    int Mode() const {
        return state | (mode & 0x0FFF);
    }

    int Level() const {
        return ((uint16_t) last_left << 16) | (uint16_t) last_right;
    }

    void Next(int16_t *left, int16_t *right) override {
        *left = *right = 0;

        if (playing && source != nullptr) {
            Produce(left, right);
        }

        last_left = *left;
        last_right = *right;
    }

private:
    /** Reads the WAVE header; false when the file is not RIFF WAVE. */
    bool ReadFormat(const std::string &name) {
        uint8_t header[12];

        if (source->Read(0, header, 12) != 12 || std::memcmp(header, "RIFF", 4) != 0 ||
            std::memcmp(header + 8, "WAVE", 4) != 0) {
            SDL_Log("EZBGM: %s is not a RIFF WAVE file", name.c_str());
            return false;
        }

        uint64_t offset = 12;
        uint8_t  chunk[8];
        bool     pcm = false;

        channels = 0;
        data_offset = 0;
        data_size = 0;

        while (source->Read(offset, chunk, 8) == 8) {
            uint32_t length = Le32(chunk + 4);

            if (std::memcmp(chunk, "fmt ", 4) == 0) {
                uint8_t format[16] = {};
                source->Read(offset + 8, format, 16);
                pcm = (format[0] | (format[1] << 8)) == 1 && (format[14] | (format[15] << 8)) == 16;
                channels = format[2] | (format[3] << 8);
                rate = Le32(format + 4);
            } else if (std::memcmp(chunk, "data", 4) == 0) {
                data_offset = offset + 8;
                data_size = std::min<uint64_t>(length, source->Size() - std::min(data_offset, source->Size()));
                break;
            }

            offset += 8 + length + (length & 1);
        }

        if (data_offset == 0) {
            SDL_Log("EZBGM: %s has no samples", name.c_str());
            return false;
        }

        if (!pcm || channels < 1 || channels > 2 || rate == 0) {
            SDL_Log("EZBGM: %s is not 16-bit PCM in mono or stereo; playing it as 48 kHz stereo", name.c_str());
            channels = 2;
            rate = kSampleRate;
        }

        total_frames = data_size / (2 * channels);
        return true;
    }

    /** The source frame `index`, or false past the end. */
    bool SourceFrame(uint64_t index, int16_t *left, int16_t *right) {
        if (index >= total_frames) {
            return false;
        }

        uint64_t offset = data_offset + index * 2 * channels;
        int16_t  samples[2] = {};

        Fetch(offset, &samples[0]);

        if (channels == 2 && !(mode & kModeMono)) {
            Fetch(offset + 2, &samples[1]);
        } else {
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
            // The module stops a channel by itself once its last sample has played.
            Stop();
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
    int                     volume = 0;
    int                     channels = 2;
    uint32_t                rate = kSampleRate;
    uint64_t                data_offset = 0;
    uint64_t                data_size = 0;
    uint64_t                total_frames = 0;
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
        case BGM_PRELOAD:
            // Reads here are direct, so there are no buffers to set up or fill.
            break;
        case BGM_QUIT:
        case BGM_CLOSE:
            channel.Close();
            break;
        case BGM_OPEN:
            result = channel.Open(NameFrom(data, size, 0, 64));
            break;
        case BGM_OPEN_LOAD:
            channel.mode = (channel.mode & ~0xFF) | ((channel.Open(NameFrom(data, size, 0, 64)) & kInfoStereo) ? 0 : kModeMono);
            break;
        case BGM_OPEN_PACK:
            result = channel.OpenPack(NameFrom(data, size, 0, 52), NameFrom(data, size, 52, 12));
            break;
        case BGM_START:
            channel.Start();
            break;
        case BGM_STOP:
            channel.Stop();
            break;
        case BGM_SEEK:
            channel.Seek(argument);
            break;
        case BGM_SET_VOLUME:
            channel.SetVolume(argument);
            break;
        case BGM_SET_VOLUME_NOW:
            channel.ApplyVolume(argument);
            break;
        case BGM_SET_MASTER_VOLUME:
            GetSpu().SetParam((uint16_t) (kMasterVolumeLeft | channel.core), (uint16_t) (argument >> 16));
            GetSpu().SetParam((uint16_t) (kMasterVolumeRight | channel.core), (uint16_t) argument);
            break;
        case BGM_SD_INIT:
            GetSoundTable().Load();
            break;
        case BGM_GET_MODE:
            result = channel.Mode();
            break;
        case BGM_SET_MODE:
            channel.mode = (channel.mode & ~0xFF) | (argument & 0xFF);
            break;
        case BGM_GET_INFO:
            result = channel.info;
            break;
        case BGM_GET_LEVEL:
            result = channel.Level();
            break;
        default:
            SDL_Log("EZBGM: unknown command 0x%X", function);
            break;
    }

    std::memset(reply, 0, sizeof(reply));
    reply[0] = result;
    return reply;
}

} // namespace audio
} // namespace pc
