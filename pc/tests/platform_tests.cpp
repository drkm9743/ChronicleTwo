// Tests of the disc, memory card, SIF and sound parts of the platform. Each
// drives the SDK functions as the game does, on data made here. (Where the
// game squeezes an EE pointer into an int argument, these keep it whole: the
// tests are not linked to stay below 2 GB.)

#include "audio/servers.h"
#include "disc/disc.h"
#include "host/host_paths.h"
#include "host/sdk.h"
#include "iop/iop.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                               \
        }                                                                             \
    } while (0)

typedef std::vector<uint8_t> Bytes;

std::filesystem::path root;

void WriteFile(const std::filesystem::path &path, const Bytes &bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

Bytes ReadFile(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Bytes Pattern(size_t size, int multiplier) {
    Bytes bytes(size);

    for (size_t i = 0; i < size; i++) {
        bytes[i] = (uint8_t) (i * multiplier);
    }

    return bytes;
}

void Put16(Bytes &bytes, size_t offset, uint32_t value) {
    bytes[offset] = (uint8_t) value;
    bytes[offset + 1] = (uint8_t) (value >> 8);
}

void Put32(Bytes &bytes, size_t offset, uint32_t value) {
    Put16(bytes, offset, value & 0xFFFF);
    Put16(bytes, offset + 2, value >> 16);
}

void PutBoth32(Bytes &bytes, size_t offset, uint32_t value) {
    Put32(bytes, offset, value);
    bytes[offset + 4] = (uint8_t) (value >> 24);
    bytes[offset + 5] = (uint8_t) (value >> 16);
    bytes[offset + 6] = (uint8_t) (value >> 8);
    bytes[offset + 7] = (uint8_t) value;
}

const Bytes kDataDat = Pattern(5000, 7);
const Bytes kHd4 = Pattern(100, 3);
const Bytes kMovie = Pattern(3 * 2048 + 10, 13);

//
// Disc images
//

/** An ISO 9660 directory record. */
Bytes Record(const std::string &name, uint32_t lsn, uint32_t size, bool directory) {
    size_t length = 33 + name.size();
    length += length & 1;
    Bytes record(length);
    record[0] = (uint8_t) length;
    PutBoth32(record, 2, lsn);
    PutBoth32(record, 10, size);
    record[25] = directory ? 2 : 0;
    record[32] = (uint8_t) name.size();
    std::memcpy(&record[33], name.data(), name.size());
    return record;
}

/** A small ISO image: DATA.DAT and DATA.HD4 at the root, MOVIE/OPEN.PSS below. */
void MakeIso(const std::filesystem::path &path) {
    const uint32_t root_lsn = 18, movie_lsn = 19, data_lsn = 20, hd4_lsn = 23, pss_lsn = 24;
    Bytes          image(28 * 2048);

    image[16 * 2048] = 1;
    std::memcpy(&image[16 * 2048 + 1], "CD001", 5);
    Bytes root_record = Record(std::string(1, '\0'), root_lsn, 2048, true);
    std::memcpy(&image[16 * 2048 + 156], root_record.data(), 34);
    image[17 * 2048] = 255;
    std::memcpy(&image[17 * 2048 + 1], "CD001", 5);

    auto directory = [&](uint32_t lsn, uint32_t parent, const std::vector<Bytes> &entries) {
        size_t offset = lsn * 2048;

        for (const Bytes &entry : {Record(std::string(1, '\0'), lsn, 2048, true), Record(std::string(1, '\1'), parent, 2048, true)}) {
            std::memcpy(&image[offset], entry.data(), entry.size());
            offset += entry.size();
        }

        for (const Bytes &entry : entries) {
            std::memcpy(&image[offset], entry.data(), entry.size());
            offset += entry.size();
        }
    };

    directory(root_lsn, root_lsn,
              {Record("DATA.DAT;1", data_lsn, (uint32_t) kDataDat.size(), false),
               Record("DATA.HD4;1", hd4_lsn, (uint32_t) kHd4.size(), false), Record("MOVIE", movie_lsn, 2048, true)});
    directory(movie_lsn, root_lsn, {Record("OPEN.PSS;1", pss_lsn, (uint32_t) kMovie.size(), false)});
    std::memcpy(&image[data_lsn * 2048], kDataDat.data(), kDataDat.size());
    std::memcpy(&image[hd4_lsn * 2048], kHd4.data(), kHd4.size());
    std::memcpy(&image[pss_lsn * 2048], kMovie.data(), kMovie.size());
    WriteFile(path, image);
}

void TestDisc(const char *kind) {
    std::printf("disc (%s)\n", kind);
    pc::disc::Unmount();
    CHECK(sceCdInit(0) == 1);

    sceCdlFILE file;
    sceCdRMode mode = {0, 1, 0, 0};
    CHECK(sceCdSearchFile(&file, "\\DATA.DAT;1") == 1);
    CHECK(file.size == kDataDat.size());

    // Two sectors from the second: the rest of the file, then zeros.
    Bytes sectors(2 * 2048, 0xEE);
    CHECK(sceCdRead(file.lsn + 1, 2, sectors.data(), &mode) == 1);
    CHECK(sceCdSync(0) == 0);
    CHECK(sceCdGetError() == 0);
    CHECK(std::memcmp(sectors.data(), &kDataDat[2048], kDataDat.size() - 2048) == 0);
    CHECK(sectors[kDataDat.size() - 2048] == 0 && sectors.back() == 0);

    CHECK(sceCdSearchFile(&file, "\\NOPE.BIN;1") == 0);

    // DATA.HD4 is lower case on the folder disc.
    int fd = sceOpen("cdrom0:\\DATA.HD4;1", SCE_RDONLY);
    CHECK(fd >= 0);
    CHECK(sceLseek(fd, 0, SCE_SEEK_END) == (int) kHd4.size());
    CHECK(sceLseek(fd, 10, SCE_SEEK_SET) == 10);
    Bytes hd4(200);
    CHECK(sceRead(fd, hd4.data(), 200) == (int) kHd4.size() - 10);
    CHECK(std::memcmp(hd4.data(), &kHd4[10], kHd4.size() - 10) == 0);
    int status = -1;
    CHECK(sceIoctl(fd, SCE_FS_EXECUTING, &status) == 0 && status == 0);
    CHECK(sceClose(fd) == 0);
    CHECK(sceOpen("cdrom0:\\NOPE.BIN;1", SCE_RDONLY) < 0);
    CHECK(sceOpen("cdrom0:\\DATA.HD4;1", SCE_WRONLY | SCE_CREAT) < 0);

    // The movie streams by sector.
    CHECK(sceCdSearchFile(&file, "\\MOVIE\\OPEN.PSS;1") == 1);
    CHECK(file.size == kMovie.size());
    CHECK(sceCdStStart(file.lsn, &mode) == 1);
    unsigned int error = 1;
    Bytes        stream(4 * 2048);
    CHECK(sceCdStRead(2, stream.data(), 1, &error) == 2 && error == 0);
    CHECK(sceCdStRead(2, &stream[2 * 2048], 1, &error) == 2 && error == 0);
    CHECK(std::memcmp(stream.data(), kMovie.data(), kMovie.size()) == 0);
    CHECK(sceCdStStop() == 1);

    pc::disc::File found;
    CHECK(pc::disc::FindAnywhere("open.pss", &found) && found.size == kMovie.size());

    sce_stat stat;
    CHECK(sceGetstat("cdrom0:\\MOVIE\\OPEN.PSS;1", &stat) == 0 && stat.st_size == kMovie.size());
}

void TestHost() {
    std::printf("host0:\n");
    pc::SetHostPath(root / "host");

    int fd = sceOpen("host0:geo_data/geo1-2.edt", SCE_WRONLY | SCE_CREAT | SCE_TRUNC);
    CHECK(fd >= 0);
    CHECK(sceWrite(fd, (void *) "hello", 5) == 5);
    CHECK(sceClose(fd) == 0);

    sce_stat stat;
    CHECK(sceGetstat("host0:geo_data/geo1-2.edt", &stat) == 0 && stat.st_size == 5);

    char text[8] = {};
    fd = sceOpen("host0:GEO_DATA/geo1-2.edt", SCE_RDONLY);
    CHECK(fd >= 0);
    CHECK(sceRead(fd, text, 8) == 5 && std::strcmp(text, "hello") == 0);
    CHECK(sceWrite(fd, text, 1) < 0);
    CHECK(sceClose(fd) == 0);
    CHECK(sceClose(fd) < 0);
    CHECK(sceOpen("host0:missing.txt", SCE_RDONLY) < 0);
}

//
// Memory card
//

struct McEntry {
    uint8_t  dates[16];
    uint32_t size;
    uint16_t attributes;
    uint8_t  reserved[10];
    char     name[32];
};

int Sync() {
    int command = -1;
    int result = -999;
    CHECK(sceMcSync(0, &command, &result) == 1);
    return result;
}

void TestMemoryCard() {
    std::printf("memory card\n");
    pc::SetMemoryCardPath(root / "memcard");
    CHECK(sceMcInit() == 0);

    int type = -1, free_size = -1, formatted = -1;
    CHECK(sceMcSync(1, NULL, NULL) == -1);
    CHECK(sceMcGetInfo(0, 0, &type, &free_size, &formatted) == 0);
    CHECK(Sync() == -1);
    CHECK(type == 2 && formatted == 1 && free_size == 8000);
    sceMcGetInfo(0, 0, &type, &free_size, &formatted);
    CHECK(Sync() == 0);
    sceMcGetInfo(1, 0, &type, &free_size, &formatted);
    CHECK(Sync() == -10 && type == 0);

    sceMcMkdir(0, 0, "/BESCES-51190DC2");
    CHECK(Sync() == 0);
    sceMcMkdir(0, 0, "/BESCES-51190DC2");
    CHECK(Sync() == -4);

    Bytes icon = Pattern(964, 5);
    sceMcOpen(0, 0, "/BESCES-51190DC2/icon.sys", 0x202);
    int fd = Sync();
    CHECK(fd >= 0);
    sceMcWrite(fd, icon.data(), (int) icon.size());
    CHECK(Sync() == (int) icon.size());
    sceMcFlush(fd);
    CHECK(Sync() == 0);
    sceMcClose(fd);
    CHECK(Sync() == 0);

    char previous[64] = {};
    sceMcChdir(0, 0, "/BESCES-51190DC2", previous);
    CHECK(Sync() == 0 && std::strcmp(previous, "/") == 0);
    sceMcOpen(0, 0, "icon.sys", 1);
    fd = Sync();
    CHECK(fd >= 0);
    Bytes back(2000);
    sceMcRead(fd, back.data(), 2000);
    CHECK(Sync() == (int) icon.size() && std::memcmp(back.data(), icon.data(), icon.size()) == 0);
    sceMcSeek(fd, 0x45, 0);
    CHECK(Sync() == 0x45);
    sceMcWrite(fd, back.data(), 1);
    CHECK(Sync() < 0);
    sceMcClose(fd);
    CHECK(Sync() == 0);
    sceMcChdir(0, 0, "/", previous);
    CHECK(Sync() == 0 && std::strcmp(previous, "/BESCES-51190DC2") == 0);

    McEntry entries[16];
    sceMcGetDir(0, 0, "/BESCES-51190DC2/*", 0, 16, reinterpret_cast<MC_DIR_ENTRY *>(entries));
    CHECK(Sync() == 3);
    CHECK(std::strcmp(entries[0].name, ".") == 0 && std::strcmp(entries[2].name, "icon.sys") == 0);
    CHECK(entries[2].size == icon.size() && entries[2].attributes == 0x8497);
    sceMcGetDir(0, 0, "/BESCES-51190*", 0, 16, reinterpret_cast<MC_DIR_ENTRY *>(entries));
    CHECK(Sync() == 1 && entries[0].attributes == 0x8427);

    sceMcRename(0, 0, (char *) "/BESCES-51190DC2/icon.sys", (char *) "icon2.sys");
    CHECK(Sync() == 0);
    sceMcDelete(0, 0, (char *) "/BESCES-51190DC2");
    CHECK(Sync() == -6);
    sceMcDelete(0, 0, (char *) "/BESCES-51190DC2/icon2.sys");
    CHECK(Sync() == 0);
    sceMcDelete(0, 0, (char *) "/BESCES-51190DC2");
    CHECK(Sync() == 0);
    sceMcOpen(0, 0, "/BESCES-51190DC2/icon.sys", 1);
    CHECK(Sync() == -4);
}

//
// IOP memory and sound
//

uint32_t ToIop(const Bytes &bytes) {
    void         *address = sceSifAllocIopHeap((unsigned int) bytes.size());
    sceSifDmaData transfer = {(void *) bytes.data(), address, (int) bytes.size(), 0};
    CHECK(address != NULL);
    CHECK(sceSifSetDma(&transfer, 1) != 0);
    CHECK(sceSifDmaStat(1) < 0);
    return pc::iop::AddressOf(address);
}

/** Two ADPCM blocks that loop: a steady 4096. */
Bytes SteadyAdpcm() {
    Bytes blocks(32, 0x11);
    blocks[0] = 0x00;
    blocks[1] = 0x04;
    blocks[16] = 0x00;
    blocks[17] = 0x03;
    return blocks;
}

std::vector<short> Render(int frames) {
    std::vector<short> out(frames * 2);
    pc::audio::Render(out.data(), frames);
    return out;
}

void TestSpu() {
    std::printf("SPU2\n");
    sceSdRemoteInit();
    sceSdRemote(1, rSdInit, 0);

    uint32_t adpcm = ToIop(SteadyAdpcm());
    sceSdRemote(1, rSdVoiceTrans, 0, SD_TRANS_MODE_WRITE, pc::iop::PointerOf(adpcm), 0x5000, 32);
    CHECK(sceSdRemote(1, rSdVoiceTransStatus, 0, SD_TRANS_STATUS_WAIT) == 1);

    sceSdRemote(1, rSdSetParam, 0x0000, 0x3FFF);
    sceSdRemote(1, rSdSetParam, 0x0100, 0x3FFF);
    sceSdRemote(1, rSdSetParam, 0x0200, 0x1000);
    sceSdRemote(1, rSdSetParam, 0x0300, 0x00FF);
    sceSdRemote(1, rSdSetParam, 0x0400, 0x1FC0);
    sceSdRemote(1, rSdSetAddr, (0x20 << 8) + 0x40, 0x5000);
    sceSdRemote(1, rSdSetSwitch, 0x15 << 8, 1);

    std::vector<short> out = Render(1000);
    CHECK(out[1998] > 4000 && out[1998] <= 4096);
    CHECK(out[1999] > 4000 && out[1999] <= 4096);
    CHECK(sceSdRemote(1, 0x8040, 0x17 << 8) & 1);

    sceSdRemote(1, rSdSetSwitch, SD_S_KOFF, 1);
    out = Render(100);
    CHECK(out[198] == 0);

    // A looping block transfer into core 1's input.
    Bytes input(2048);

    for (int i = 0; i < 256; i++) {
        Put16(input, i * 2, 1000);
        Put16(input, 512 + i * 2, (uint16_t) -1000);
        Put16(input, 1024 + i * 2, 2000);
        Put16(input, 1536 + i * 2, (uint16_t) -2000);
    }

    uint32_t buffer = ToIop(input);
    sceSdRemote(1, 0x80E0, 1, 0x10, buffer, 2048);
    out = Render(10);
    CHECK(out[18] > 950 && out[18] <= 1000 && out[19] < -950 && out[19] >= -1000);
    CHECK((sceSdRemote(1, 0x8100, 1, 0) & 0xFFFFFF) == (int) buffer);
    out = Render(300);
    CHECK(out[598] > 1950 && out[598] <= 2000);
    CHECK(sceSdRemote(1, 0x8100, 1, 0) == (int) ((1 << 24) | (buffer + 1024)));
    CHECK(sceSdRemote(1, 0x80E0, 1, 2, 0, 0) == (int) (buffer + 1024));
    out = Render(10);
    CHECK(out[18] == 0);
}

void Wave(const std::filesystem::path &path, int channels, int rate, int frames, short value) {
    Bytes bytes(44 + frames * channels * 2);
    std::memcpy(&bytes[0], "RIFF", 4);
    Put32(bytes, 4, (uint32_t) bytes.size() - 8);
    std::memcpy(&bytes[8], "WAVEfmt ", 8);
    Put32(bytes, 16, 16);
    Put16(bytes, 20, 1);
    Put16(bytes, 22, channels);
    Put32(bytes, 24, rate);
    Put32(bytes, 28, rate * channels * 2);
    Put16(bytes, 32, channels * 2);
    Put16(bytes, 34, 16);
    std::memcpy(&bytes[36], "data", 4);
    Put32(bytes, 40, frames * channels * 2);

    for (int i = 0; i < frames * channels; i++) {
        Put16(bytes, 44 + i * 2, (uint16_t) value);
    }

    WriteFile(path, bytes);
}

sceSifClientData bgm_client;

int Bgm(int command, intptr_t argument) {
    int reply[16] = {};

    if ((command & 0xFFF0) == 0x8020) {
        char block[64] = {};
        std::strncpy(block, reinterpret_cast<const char *>(argument), 63);
        sceSifCallRpc(&bgm_client, command, 1, block, 64, reply, 64, NULL, NULL);
    } else {
        int send[4] = {(int) argument};
        sceSifCallRpc(&bgm_client, command, 0, send, 16, reply, 64, NULL, NULL);
    }

    return reply[0];
}

void TestEzBgm() {
    std::printf("EZBGM\n");
    Wave(pc::DiscPath() / "VOICE" / "TEST.WV", 1, 24000, 4800, 2000);
    pc::disc::Unmount();

    sceSifInitRpc(0);
    CHECK(sceSifBindRpc(&bgm_client, 0x12345, 0) == 0 && bgm_client.server != NULL);

    const char *name = "cdrom0:\\VOICE\\TEST.WV;1";
    Bgm(0x80, 0);
    CHECK(Bgm(0x8020, (intptr_t) name) == 9600);
    CHECK(Bgm(0x80D0, 0) == 9600);
    Bgm(0x8000, 0x3000);
    Bgm(0x80C0, 0x10);
    Bgm(0x40, 0);
    Bgm(0x80, (0x7FFF << 16) | 0x7FFF);
    Bgm(0x50, 0);
    CHECK((Bgm(0x80B0, 0) & ~0xFFF) == 0x1000);

    std::vector<short> out = Render(1000);
    CHECK(out[1998] > 1900 && out[1998] <= 2000 && out[1999] > 1900);
    CHECK((Bgm(0x80E0, 0) & 0xFFFF) == 2000);
    out = Render(1400);
    CHECK((Bgm(0x80B0, 0) & ~0xFFF) == 0x8000);
    out = Render(8000);
    CHECK((Bgm(0x80B0, 0) & ~0xFFF) == 0);
    CHECK(out[15998] == 0);
    Bgm(0x60, 0);
    Bgm(0x30, 0);
    Bgm(0x10, 0);

    // A bare name is a member of SOUND.DAT, found through SOUND.HD3.
    Wave(pc::DiscPath() / "MEMBER.WAV", 2, 48000, 1000, 3000);
    Bytes member = ReadFile(pc::DiscPath() / "MEMBER.WAV");
    Bytes dat(2 * 2048 + member.size());
    std::memcpy(&dat[2 * 2048], member.data(), member.size());
    WriteFile(pc::DiscPath() / "SOUND.DAT", dat);

    Bytes hd3(64);
    Put32(hd3, 0, 32);
    Put32(hd3, 4, (uint32_t) member.size());
    Put32(hd3, 8, 2);
    Put32(hd3, 12, 1);
    std::memcpy(&hd3[32], "0000010.wav", 11);
    WriteFile(pc::DiscPath() / "SOUND.HD3", hd3);
    pc::disc::Unmount();

    const char *bare = "0000010.wav";
    CHECK(Bgm(0x8021, (intptr_t) bare) == (4000 | 1));
    Bgm(0x81, (0x7FFF << 16) | 0x7FFF);
    Bgm(0x51, 0);
    CHECK((Bgm(0x80B1, 0) & ~0xFFF) == 0x1000);
    out = Render(100);
    CHECK(out[2 * 50 + 1] > 2900 && out[2 * 50 + 1] <= 3000);

    // Stopping silences the channel and clears its state.
    Bgm(0x61, 0);
    CHECK((Bgm(0x80B1, 0) & ~0xFFF) == 0);
    out = Render(10);
    CHECK(out[19] == 0);

    // What is not RIFF WAVE is refused, and a name not on the list is not opened.
    CHECK(Bgm(0x8021, (intptr_t) "cdrom0:\\SOUND.HD3;1") == -1);
    CHECK(Bgm(0x8021, (intptr_t) "missing.wav") == 0);
    Bgm(0x31, 0);
}

/** A sound bank header: one program whose one sample (base note 60) plays for every key. */
Bytes MakeHd() {
    Bytes hd(16 + 0x40);
    std::memcpy(&hd[0], "IECSsreV", 8);
    Put32(hd, 8, 16);
    std::memcpy(&hd[16], "IECSdaeH", 8);
    Put32(hd, 24, 0x40);

    auto chunk = [&](const char *type, const Bytes &entry) {
        size_t offset = hd.size();
        hd.resize(offset + 20 + entry.size());
        std::memcpy(&hd[offset], "IECS", 4);
        std::memcpy(&hd[offset + 4], type, 4);
        Put32(hd, offset + 8, (uint32_t) (20 + entry.size()));
        Put32(hd, offset + 12, 0);
        Put32(hd, offset + 16, 20);
        std::memcpy(&hd[offset + 20], entry.data(), entry.size());
    };

    Bytes vag(8);
    Put32(vag, 0, 0);
    Put16(vag, 4, 48000);
    vag[6] = 1;
    chunk("igaV", vag);

    Bytes sample(0x2A);
    sample[4] = 127;
    sample[0x0B] = 60;
    sample[0x10] = 127;
    Put16(sample, 0x12, 0x00FF);
    Put16(sample, 0x14, 0x1FC0);
    chunk("lpmS", sample);

    Bytes set(6);
    set[2] = 127;
    set[3] = 1;
    chunk("tesS", set);

    Bytes program(0x24 + 0x14);
    Put32(program, 0, 0x24);
    program[4] = 1;
    program[5] = 0x14;
    program[6] = 127;
    program[0x24 + 4] = 127;
    program[0x24 + 0x10] = 127;
    chunk("gorP", program);

    Put32(hd, 28, (uint32_t) hd.size());
    return hd;
}

/** A sequence: middle C for half a second (480 ticks at 120 BPM), then the end. */
Bytes MakeSq() {
    Bytes events = {0x00, 0x90, 60, 100, 0x83, 0x60, 0x80, 60, 0, 0x00, 0xFF, 0x2F, 0x00};
    Bytes sq(16 + 20 + 6);
    std::memcpy(&sq[0], "IECSsreV", 8);
    Put32(sq, 8, 16);
    std::memcpy(&sq[16], "IECSidiM", 8);
    Put32(sq, 24, (uint32_t) (20 + 6 + events.size()));
    Put32(sq, 28, 0);
    Put32(sq, 32, 20);
    Put32(sq, 36, 6);
    Put16(sq, 40, 480);
    sq.insert(sq.end(), events.begin(), events.end());
    return sq;
}

sceSifClientData midi_client;

int Midi(int command, intptr_t argument) {
    int reply[16] = {};

    if (command & 0x1000) {
        sceSifCallRpc(&midi_client, command, 0, reinterpret_cast<void *>(argument), 64, reply, 64, NULL, NULL);
    } else {
        int send[4] = {(int) argument};
        sceSifCallRpc(&midi_client, command, 0, send, 16, reply, 64, NULL, NULL);
    }

    return reply[0];
}

struct BankRequest {
    int32_t bank_no;
    void   *hd_address;
    void   *bd_address;
    int32_t bd_size;
    int32_t spu_address;
    uint8_t padding[0x40];
};

struct StreamBuffer {
    int32_t size;
    int32_t length;
    uint8_t messages[0x1F8];
};

int Peak(const std::vector<short> &out) {
    int peak = 0;

    for (short sample : out) {
        peak = std::max(peak, std::abs((int) sample));
    }

    return peak;
}

void TestEzMidi() {
    std::printf("EZMIDI\n");
    CHECK(sceSifBindRpc(&midi_client, 0x12346, 0) == 0 && midi_client.server != NULL);
    uint32_t stream_address = (uint32_t) Midi(0x8010, 0x4000);
    CHECK(stream_address >= pc::iop::kMemoryBase);

    sceSdRemote(1, rSdInit, 0);
    Bytes    hd = MakeHd();
    uint32_t hd_address = ToIop(hd);
    uint32_t bd_address = ToIop(SteadyAdpcm());
    sceSdRemote(1, rSdVoiceTrans, 1, SD_TRANS_MODE_WRITE, pc::iop::PointerOf(bd_address), 0x5210, 32);

    BankRequest bank = {0, pc::iop::PointerOf(hd_address), pc::iop::PointerOf(bd_address), 32, 0x5210, {}};

    for (int port : {0, 7}) {
        Midi(port + 0x20, 0);
        CHECK(Midi(port + 0x9050, (intptr_t) &bank) == 0);
        Midi(port + 0xA0, 0x3040);
    }

    // The sequence on port 0: a note for half a second.
    uint32_t sq_address = ToIop(MakeSq());
    Midi(0x40, (int) sq_address);
    Midi(0xB0, 256);
    Midi(0x30, 0);
    Midi(0x00, 0);
    CHECK(Peak(Render(12000)) > 1000);
    Render(13000);
    CHECK(Peak(Render(2000)) == 0);

    // A sound effect on port 7, through the MIDI stream input.
    static StreamBuffer buffer;
    sceCslBuffCtx       contexts[1] = {
        {0, &buffer}
    };
    sceCslBuffGrp groups[2] = {
        {0, NULL    },
        {1, contexts}
    };
    sceCslCtx ctx = {2, groups, NULL, NULL, NULL};
    buffer.size = sizeof(buffer);
    buffer.length = 0;
    CHECK(sceMSIn_Init(&ctx) == 0);

    auto send = [&]() {
        sceSifDmaData transfer = {&buffer, pc::iop::PointerOf(stream_address), (int) sizeof(buffer), 0};
        sceSifSetDma(&transfer, 1);
        buffer.length = 0;
    };

    unsigned char volume[5] = {0xF9, 0, 0, 127, 0};
    unsigned char play[7] = {0xFD, 0x10, 0, 60, 1, 127, 0};
    CHECK(sceMSIn_PutMsg(&ctx, 0, 0xB0) == 0);
    CHECK(sceMSIn_PutMsg(&ctx, 0, 0xC0) == 0);
    CHECK(sceMSIn_PutHsMsg(&ctx, 0, volume) == 0);
    CHECK(sceMSIn_PutHsMsg(&ctx, 0, play) == 0);
    CHECK(buffer.length == 3 + 2 + 5 + 7);
    send();
    CHECK(Peak(Render(4800)) > 1000);

    unsigned char stop[7] = {0xFD, 0x10, 0, 60, 1, 0, 0};
    sceMSIn_PutHsMsg(&ctx, 0, stop);
    send();
    Render(4800);
    CHECK(Peak(Render(2000)) == 0);

    Midi(0x80F0, 0);
}

} // namespace

int main() {
    root = std::filesystem::temp_directory_path() / ("chronicletwo-tests-" + std::to_string(SDL_GetTicksNS()));
    std::filesystem::create_directories(root);
    pc::audio::DisableOutput();

    WriteFile(root / "folder" / "DATA.DAT", kDataDat);
    WriteFile(root / "folder" / "data.hd4", kHd4);
    WriteFile(root / "folder" / "MOVIE" / "OPEN.PSS", kMovie);
    MakeIso(root / "disc.iso");

    pc::SetDiscPath(root / "disc.iso");
    TestDisc("ISO image");
    pc::SetDiscPath(root / "folder");
    TestDisc("folder");
    TestHost();
    TestMemoryCard();
    TestSpu();
    TestEzBgm();
    TestEzMidi();

    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::printf(failures == 0 ? "all passed\n" : "%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
