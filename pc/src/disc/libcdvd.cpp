// libcdvd on the PC: the disc of disc.h, read at once. Every command has
// finished by the time it returns, so sceCdSync never waits and the streaming
// functions read straight from the disc.

#include "../host/sdk.h"
#include "disc.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <mutex>

namespace {

/** sceCdGetError codes. */
const int kErrorNone = 0x00;
const int kErrorRead = 0x30;
const int kErrorNoDisc = 0x31;

/** sceCdDiskReady: the drive is ready. */
const int kDiskReady = 2;

struct Drive {
    std::mutex lock;
    int        error = kErrorNone;
    uint32_t   stream_lsn = 0; /**< Next sector the stream reads. */
    bool       streaming = false;
};

Drive &GetDrive() {
    static Drive drive;
    return drive;
}

void SetError(int error) {
    std::lock_guard<std::mutex> guard(GetDrive().lock);
    GetDrive().error = error;
}

} // namespace

extern "C" {

int sceCdInit(int mode) {
    // Mode 5 shuts the library down; any other mode starts it.
    if (mode != 5) {
        pc::disc::Mount();
    }

    return 1;
}

int sceCdMmode(int media) {
    (void) media;
    return 1;
}

int sceCdDiskReady(int mode) {
    (void) mode;
    return kDiskReady;
}

int sceCdSearchFile(sceCdlFILE *file, const char *name) {
    pc::disc::File found;

    if (!pc::disc::Find(name, &found) || found.directory) {
        SetError(pc::disc::Mount() ? kErrorNone : kErrorNoDisc);
        return 0;
    }

    std::memset(file, 0, sizeof(*file));
    file->lsn = found.lsn;
    file->size = found.size;
    std::strncpy(file->name, (found.name + ";1").c_str(), sizeof(file->name) - 1);
    SetError(kErrorNone);
    return 1;
}

int sceCdSeek(u_int lsn) {
    (void) lsn;
    return 1;
}

int sceCdRead(u_int lsn, u_int sectors, void *buffer, sceCdRMode *mode) {
    (void) mode;
    SetError(pc::disc::ReadSectors(lsn, sectors, buffer) ? kErrorNone : kErrorRead);
    return 1;
}

int sceCdSync(int mode) {
    (void) mode;
    return 0;
}

int sceCdGetError(void) {
    std::lock_guard<std::mutex> guard(GetDrive().lock);
    return GetDrive().error;
}

int sceCdBreak(void) {
    return 1;
}

int sceCdStInit(int sectors, int banks, void *buffer) {
    (void) sectors;
    (void) banks;
    (void) buffer;
    return 1;
}

int sceCdStStart(unsigned int sector, sceCdRMode *mode) {
    (void) mode;
    std::lock_guard<std::mutex> guard(GetDrive().lock);
    GetDrive().stream_lsn = sector;
    GetDrive().streaming = true;
    return 1;
}

int sceCdStSeekF(unsigned int sector) {
    std::lock_guard<std::mutex> guard(GetDrive().lock);
    GetDrive().stream_lsn = sector;
    return 1;
}

int sceCdStStop(void) {
    std::lock_guard<std::mutex> guard(GetDrive().lock);
    GetDrive().streaming = false;
    return 1;
}

int sceCdStRead(unsigned int sectors, void *buffer, unsigned int mode, unsigned int *error) {
    (void) mode;
    Drive                      &drive = GetDrive();
    std::lock_guard<std::mutex> guard(drive.lock);

    if (!drive.streaming) {
        *error = kErrorRead;
        return 0;
    }

    if (!pc::disc::ReadSectors(drive.stream_lsn, sectors, buffer)) {
        *error = kErrorRead;
        return 0;
    }

    drive.stream_lsn += sectors;
    *error = kErrorNone;
    return (int) sectors;
}

} // extern "C"
