#include "../host/host_paths.h"
#include "disc.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

namespace pc {
namespace disc {

namespace {

/** Splits a disc path into its components, dropping separators, ";1" and a bare trailing '.'. */
std::vector<std::string> SplitPath(const std::string &path) {
    std::vector<std::string> components;
    size_t                   start = 0;

    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);

        if (end == std::string::npos) {
            end = path.size();
        }

        std::string component = path.substr(start, end - start);
        size_t      version = component.find(';');

        if (version != std::string::npos) {
            component.erase(version);
        }

        if (!component.empty() && component.back() == '.') {
            component.pop_back();
        }

        if (!component.empty()) {
            components.push_back(component);
        }

        start = end + 1;
    }

    return components;
}

std::string LowerCase(std::string text) {
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        }
    }

    return text;
}

class Backend {
public:
    virtual ~Backend() {}

    virtual bool Find(const std::vector<std::string> &components, File *file) = 0;
    virtual bool Read(uint64_t offset, void *buffer, uint32_t size) = 0;

    /** Every file on the disc, by lower-case name, built the first time it is needed. */
    bool FindAnywhere(const std::string &name, File *file) {
        if (!indexed) {
            BuildIndex();
            indexed = true;
        }

        std::map<std::string, File>::iterator it = index.find(LowerCase(SplitPath(name).back()));

        if (it == index.end()) {
            return false;
        }

        *file = it->second;
        return true;
    }

protected:
    virtual void BuildIndex() = 0;

    std::map<std::string, File> index;

private:
    bool indexed = false;
};

/** An ISO 9660 image of 2048-byte sectors, as a DVD image is. */
class IsoBackend : public Backend {
public:
    bool Open(const std::filesystem::path &path) {
        stream.open(path, std::ios::binary);

        if (!stream) {
            return false;
        }

        // The primary volume descriptor is in sector 16; the root directory's record is at 156.
        uint8_t descriptor[kSectorSize];

        if (!Read(16 * (uint64_t) kSectorSize, descriptor, kSectorSize) || descriptor[0] != 1 ||
            std::memcmp(&descriptor[1], "CD001", 5) != 0) {
            SDL_Log("%s is not an ISO 9660 image", Utf8FromPath(path).c_str());
            return false;
        }

        root.lsn = Le32(&descriptor[156 + 2]);
        root.size = Le32(&descriptor[156 + 10]);
        root.directory = true;
        return true;
    }

    bool Find(const std::vector<std::string> &components, File *file) override {
        File current = root;

        for (const std::string &component : components) {
            if (!current.directory || !FindInDirectory(current, component, &current)) {
                return false;
            }
        }

        *file = current;
        return true;
    }

    bool Read(uint64_t offset, void *buffer, uint32_t size) override {
        stream.clear();
        stream.seekg((std::streamoff) offset);
        stream.read(static_cast<char *>(buffer), size);
        std::streamsize got = stream.gcount();

        if (got < (std::streamsize) size) {
            std::memset(static_cast<char *>(buffer) + got, 0, size - got);
        }

        return got > 0 || size == 0;
    }

protected:
    void BuildIndex() override {
        std::vector<File> pending(1, root);

        while (!pending.empty()) {
            File directory = pending.back();
            pending.pop_back();
            ForEachEntry(directory, [&](const File &entry) {
                if (entry.directory) {
                    pending.push_back(entry);
                } else {
                    index.emplace(LowerCase(entry.name), entry);
                }

                return false;
            });
        }
    }

private:
    static uint32_t Le32(const uint8_t *bytes) {
        return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
    }

    /** Calls `visit` on each entry of a directory but "." and "..", until it gives true. */
    template <typename Visit>
    void ForEachEntry(const File &directory, Visit visit) {
        std::vector<uint8_t> records(directory.size);

        if (directory.size == 0 || !Read((uint64_t) directory.lsn * kSectorSize, records.data(), directory.size)) {
            return;
        }

        uint32_t offset = 0;

        while (offset < directory.size) {
            uint8_t length = records[offset];

            // Records do not cross sectors; a zero length pads to the next one.
            if (length == 0) {
                offset = (offset / kSectorSize + 1) * kSectorSize;
                continue;
            }

            if (offset + length > directory.size || length < 34) {
                break;
            }

            const uint8_t *record = &records[offset];
            uint8_t        name_length = record[32];
            offset += length;

            if (name_length == 1 && (record[33] == 0 || record[33] == 1)) {
                continue;
            }

            File entry;
            entry.lsn = Le32(&record[2]);
            entry.size = Le32(&record[10]);
            entry.directory = (record[25] & 2) != 0;
            entry.name = SplitPath(std::string(reinterpret_cast<const char *>(&record[33]), name_length)).back();

            if (visit(entry)) {
                return;
            }
        }
    }

    bool FindInDirectory(const File &directory, const std::string &name, File *found) {
        bool matched = false;

        ForEachEntry(directory, [&](const File &entry) {
            if (NamesEqualNoCase(entry.name, name)) {
                *found = entry;
                matched = true;
            }

            return matched;
        });

        return matched;
    }

    std::ifstream stream;
    File          root;
};

/** A folder holding the extracted disc. */
class FolderBackend : public Backend {
public:
    explicit FolderBackend(const std::filesystem::path &root) : root(root) {}

    bool Find(const std::vector<std::string> &components, File *file) override {
        std::string relative;

        for (const std::string &component : components) {
            relative += component;
            relative += '/';
        }

        std::filesystem::path path;

        if (!ResolveNoCase(root, relative, &path)) {
            return false;
        }

        std::error_code error;

        if (std::filesystem::is_directory(path, error)) {
            file->lsn = 0;
            file->size = 0;
            file->name = components.empty() ? std::string() : Utf8FromPath(path.filename());
            file->directory = true;
            return true;
        }

        return Place(path, file);
    }

    bool Read(uint64_t offset, void *buffer, uint32_t size) override {
        uint8_t *out = static_cast<uint8_t *>(buffer);

        while (size > 0) {
            uint32_t lsn = (uint32_t) (offset / kSectorSize);
            uint32_t chunk = size;
            bool     filled = false;

            // The file whose run of sectors holds this one, if any.
            std::map<uint32_t, Placed>::iterator it = by_lsn.upper_bound(lsn);

            if (it != by_lsn.begin()) {
                --it;
                uint64_t start = (uint64_t) it->first * kSectorSize;
                uint64_t end = start + it->second.size;

                if (offset < end) {
                    chunk = (uint32_t) std::min<uint64_t>(size, end - offset);

                    if (!ReadHostFile(it->second, offset - start, out, chunk)) {
                        return false;
                    }

                    filled = true;
                } else if (it != std::prev(by_lsn.end())) {
                    // In the gap after a file: zeros up to the next file.
                    uint64_t next = (uint64_t) std::next(it)->first * kSectorSize;
                    chunk = (uint32_t) std::min<uint64_t>(size, next - offset);
                }
            } else if (!by_lsn.empty()) {
                uint64_t next = (uint64_t) by_lsn.begin()->first * kSectorSize;
                chunk = (uint32_t) std::min<uint64_t>(size, next - offset);
            }

            if (!filled) {
                std::memset(out, 0, chunk);
            }

            out += chunk;
            offset += chunk;
            size -= chunk;
        }

        return true;
    }

protected:
    void BuildIndex() override {
        std::error_code error;

        for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end;
             it.increment(error)) {
            if (it->is_regular_file(error)) {
                File file;

                if (Place(it->path(), &file)) {
                    index.emplace(LowerCase(file.name), file);
                }
            }
        }
    }

private:
    /** The first sector given out; the ones before are the image's system area and volume descriptors. */
    static const uint32_t kFirstLsn = 0x400;

    /** Sectors left free after each file, so reading past its end finds nothing. */
    static const uint32_t kGapSectors = 16;

    struct Placed {
        std::filesystem::path          path;
        uint32_t                       size;
        std::shared_ptr<std::ifstream> stream;
    };

    /** Gives a file its run of sectors, the first time it is seen. */
    bool Place(const std::filesystem::path &path, File *file) {
        std::string                               key = Utf8FromPath(path);
        std::map<std::string, uint32_t>::iterator known = by_path.find(key);
        uint32_t                                  lsn;

        if (known != by_path.end()) {
            lsn = known->second;
        } else {
            std::error_code error;
            uintmax_t       size = std::filesystem::file_size(path, error);

            if (error || size > 0xFFFFFFFFu) {
                return false;
            }

            lsn = next_lsn;
            next_lsn += (uint32_t) ((size + kSectorSize - 1) / kSectorSize) + kGapSectors;
            by_path[key] = lsn;
            by_lsn[lsn] = Placed{path, (uint32_t) size, nullptr};
        }

        file->lsn = lsn;
        file->size = by_lsn[lsn].size;
        file->name = Utf8FromPath(path.filename());
        file->directory = false;
        return true;
    }

    bool ReadHostFile(Placed &placed, uint64_t offset, uint8_t *out, uint32_t size) {
        if (placed.stream == nullptr) {
            placed.stream = std::make_shared<std::ifstream>(placed.path, std::ios::binary);
        }

        std::ifstream &stream = *placed.stream;
        stream.clear();
        stream.seekg((std::streamoff) offset);
        stream.read(reinterpret_cast<char *>(out), size);
        std::streamsize got = stream.gcount();

        if (got < (std::streamsize) size) {
            std::memset(out + got, 0, size - got);
        }

        return got == (std::streamsize) size;
    }

    std::filesystem::path           root;
    std::map<std::string, uint32_t> by_path;
    std::map<uint32_t, Placed>      by_lsn;
    uint32_t                        next_lsn = kFirstLsn;
};

struct State {
    std::mutex               lock;
    std::unique_ptr<Backend> backend;
    bool                     tried = false;
};

State &GetState() {
    static State state;
    return state;
}

/** Opens the disc if no attempt has been made yet; the lock is held. */
Backend *Mounted(State &state) {
    if (state.tried) {
        return state.backend.get();
    }

    state.tried = true;
    std::filesystem::path path = DiscPath();
    std::error_code       error;

    if (std::filesystem::is_directory(path, error)) {
        state.backend.reset(new FolderBackend(path));
    } else if (std::filesystem::is_regular_file(path, error)) {
        std::unique_ptr<IsoBackend> iso(new IsoBackend());

        if (iso->Open(path)) {
            state.backend = std::move(iso);
        }
    }

    if (state.backend == nullptr) {
        SDL_Log("No disc at %s: set DC2_DISC to the ISO image or to the folder holding the extracted disc",
                Utf8FromPath(path).c_str());
    } else {
        SDL_Log("Disc: %s", Utf8FromPath(path).c_str());
    }

    return state.backend.get();
}

} // namespace

bool Mount() {
    State                      &state = GetState();
    std::lock_guard<std::mutex> guard(state.lock);
    return Mounted(state) != NULL;
}

void Unmount() {
    State                      &state = GetState();
    std::lock_guard<std::mutex> guard(state.lock);
    state.backend.reset();
    state.tried = false;
}

bool Find(const std::string &path, File *file) {
    State                      &state = GetState();
    std::lock_guard<std::mutex> guard(state.lock);
    Backend                    *backend = Mounted(state);
    return backend != NULL && backend->Find(SplitPath(path), file);
}

bool FindAnywhere(const std::string &name, File *file) {
    State                      &state = GetState();
    std::lock_guard<std::mutex> guard(state.lock);
    Backend                    *backend = Mounted(state);
    return backend != NULL && !SplitPath(name).empty() && backend->FindAnywhere(name, file);
}

bool Read(uint64_t offset, void *buffer, uint32_t size) {
    State                      &state = GetState();
    std::lock_guard<std::mutex> guard(state.lock);
    Backend                    *backend = Mounted(state);
    return backend != NULL && backend->Read(offset, buffer, size);
}

} // namespace disc
} // namespace pc
