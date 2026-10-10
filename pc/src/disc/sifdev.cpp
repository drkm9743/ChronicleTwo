// The SIF file functions (sceOpen and the rest) on the PC. Two devices are
// served: the disc ("cdrom0:", read-only) and the development host ("host0:",
// a folder on the PC). A name with no device goes to the host, as on the
// development kit. Every call finishes before it returns, so SCE_NOWAIT
// changes nothing and sceIoctl always reports the descriptor idle.

#include "../host/host_paths.h"
#include "../host/sdk.h"
#include "disc.h"

#include <SDL3/SDL.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

namespace {

const int kOpenAccessMask = 0x0003;
const int kOpenAppend = 0x0100;

/** File mode bits of sce_stat. */
const unsigned int kStatDirectory = 0x1000;
const unsigned int kStatRegular = 0x2000;

enum Device {
    DEVICE_NONE,
    DEVICE_DISC,
    DEVICE_HOST,
};

struct Handle {
    Device                        device;
    pc::disc::File                disc_file; /**< DEVICE_DISC: the file on the disc. */
    std::unique_ptr<std::fstream> host_file; /**< DEVICE_HOST: the open host file. */
    int64_t                       position;
    int64_t                       size;
    bool                          writable;
    bool                          append;
};

struct Files {
    std::mutex            lock;
    std::map<int, Handle> handles;
    int                   next_fd = 3;
};

Files &GetFiles() {
    static Files files;
    return files;
}

/** Splits "device:path" into the device and the path on it. */
Device ParseName(const char *name, std::string *path) {
    const char *colon = std::strchr(name, ':');

    if (colon == NULL) {
        *path = name;
        return DEVICE_HOST;
    }

    std::string device(name, colon - name);
    *path = colon + 1;

    if (device == "cdrom0" || device == "cdrom") {
        return DEVICE_DISC;
    }

    if (device == "host0" || device == "host") {
        return DEVICE_HOST;
    }

    SDL_Log("sceOpen: device %s: is not available on the PC", device.c_str());
    return DEVICE_NONE;
}

/** The host file a host0: name stands for, matched in either case where it exists. */
std::filesystem::path HostFile(const std::string &path) {
    std::filesystem::path resolved;
    pc::ResolveNoCase(pc::HostPath(), path, &resolved);
    return resolved;
}

} // namespace

extern "C" {

void sceFsReset(void) {}

int sceOpen(const char *name, int flags, ...) {
    std::string path;
    Handle      handle = {};
    Device      device = ParseName(name, &path);
    int         access = flags & kOpenAccessMask;

    handle.device = device;
    handle.writable = access == SCE_WRONLY || access == (SCE_RDONLY | SCE_WRONLY);
    handle.append = (flags & kOpenAppend) != 0;

    if (device == DEVICE_DISC) {
        if (handle.writable) {
            return -EROFS;
        }

        if (!pc::disc::Find(path, &handle.disc_file) || handle.disc_file.directory) {
            return -ENOENT;
        }

        handle.size = handle.disc_file.size;
    } else if (device == DEVICE_HOST) {
        std::filesystem::path file = HostFile(path);
        std::error_code       error;
        bool                  exists = std::filesystem::exists(file, error);

        if (!exists && !(flags & SCE_CREAT)) {
            return -ENOENT;
        }

        if (!exists || (flags & SCE_TRUNC)) {
            std::filesystem::create_directories(file.parent_path(), error);
            std::ofstream create(file, std::ios::binary | std::ios::trunc);

            if (!create) {
                return -EACCES;
            }
        }

        std::ios::openmode mode = std::ios::binary | std::ios::in;

        if (handle.writable) {
            mode |= std::ios::out;
        }

        handle.host_file.reset(new std::fstream(file, mode));

        if (!*handle.host_file) {
            return -EACCES;
        }

        handle.size = (int64_t) std::filesystem::file_size(file, error);
    } else {
        return -ENODEV;
    }

    Files                      &files = GetFiles();
    std::lock_guard<std::mutex> guard(files.lock);
    int                         fd = files.next_fd++;
    files.handles[fd] = std::move(handle);
    return fd;
}

int sceClose(int fd) {
    Files                      &files = GetFiles();
    std::lock_guard<std::mutex> guard(files.lock);
    return files.handles.erase(fd) != 0 ? 0 : -EBADF;
}

int sceLseek(int fd, int offset, int whence) {
    Files                          &files = GetFiles();
    std::lock_guard<std::mutex>     guard(files.lock);
    std::map<int, Handle>::iterator it = files.handles.find(fd);

    if (it == files.handles.end()) {
        return -EBADF;
    }

    Handle &handle = it->second;
    int64_t position;

    switch (whence) {
        case SCE_SEEK_SET:
            position = offset;
            break;
        case SCE_SEEK_CUR:
            position = handle.position + offset;
            break;
        case SCE_SEEK_END:
            position = handle.size + offset;
            break;
        default:
            return -EINVAL;
    }

    if (position < 0) {
        return -EINVAL;
    }

    handle.position = position;
    return (int) position;
}

int sceRead(int fd, void *buffer, int size) {
    Files                          &files = GetFiles();
    std::lock_guard<std::mutex>     guard(files.lock);
    std::map<int, Handle>::iterator it = files.handles.find(fd);

    if (it == files.handles.end() || size < 0) {
        return -EBADF;
    }

    Handle &handle = it->second;
    int64_t left = handle.size - handle.position;
    int     count = (int) (left < size ? (left > 0 ? left : 0) : size);

    if (count == 0) {
        return 0;
    }

    if (handle.device == DEVICE_DISC) {
        uint64_t start = (uint64_t) handle.disc_file.lsn * pc::disc::kSectorSize + handle.position;

        if (!pc::disc::Read(start, buffer, count)) {
            return -EIO;
        }
    } else {
        std::fstream &file = *handle.host_file;
        file.clear();
        file.seekg(handle.position);
        file.read(static_cast<char *>(buffer), count);
        count = (int) file.gcount();
    }

    handle.position += count;
    return count;
}

int sceWrite(int fd, void *buffer, int size) {
    Files                          &files = GetFiles();
    std::lock_guard<std::mutex>     guard(files.lock);
    std::map<int, Handle>::iterator it = files.handles.find(fd);

    if (it == files.handles.end() || size < 0) {
        return -EBADF;
    }

    Handle &handle = it->second;

    if (handle.device != DEVICE_HOST || !handle.writable) {
        return -EBADF;
    }

    if (handle.append) {
        handle.position = handle.size;
    }

    std::fstream &file = *handle.host_file;
    file.clear();
    file.seekp(handle.position);
    file.write(static_cast<const char *>(buffer), size);
    file.flush();

    if (!file) {
        return -EIO;
    }

    handle.position += size;

    if (handle.position > handle.size) {
        handle.size = handle.position;
    }

    return size;
}

int sceIoctl(int fd, int request, void *argument) {
    Files                      &files = GetFiles();
    std::lock_guard<std::mutex> guard(files.lock);

    if (files.handles.find(fd) == files.handles.end()) {
        return -EBADF;
    }

    if (request == SCE_FS_EXECUTING && argument != NULL) {
        *static_cast<int *>(argument) = 0;
    }

    return 0;
}

int sceGetstat(const char *name, struct sce_stat *stat) {
    std::string path;
    Device      device = ParseName(name, &path);

    std::memset(stat, 0, sizeof(*stat));

    if (device == DEVICE_DISC) {
        pc::disc::File file;

        if (!pc::disc::Find(path, &file)) {
            return -ENOENT;
        }

        stat->st_mode = (file.directory ? kStatDirectory : kStatRegular) | 0x16D;
        stat->st_size = file.size;
        return 0;
    }

    if (device == DEVICE_HOST) {
        std::filesystem::path        file = HostFile(path);
        std::error_code              error;
        std::filesystem::file_status status = std::filesystem::status(file, error);

        if (error || !std::filesystem::exists(status)) {
            return -ENOENT;
        }

        stat->st_mode = (std::filesystem::is_directory(status) ? kStatDirectory : kStatRegular) | 0x1FF;

        if (std::filesystem::is_regular_file(status)) {
            uintmax_t size = std::filesystem::file_size(file, error);
            stat->st_size = (unsigned int) size;
            stat->st_hisize = (unsigned int) (size >> 32);
        }

        return 0;
    }

    return -ENODEV;
}

} // extern "C"
