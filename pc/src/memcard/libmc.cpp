// libmc on the PC: each memory card is a folder under MemoryCardPath(),
// "slot1" for the card in port 0 and "slot2" for port 1, holding the card's
// directories and files as they are. The card in slot 1 is always inserted
// (its folder is made when needed); the one in slot 2 is inserted when its
// folder exists.
//
// A command runs to the end when it is issued, and sceMcSync hands back its
// result the first time it is asked, as a finished command's result is
// handed back on the PS2.

#include "../host/host_paths.h"
#include "../host/sdk.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace {

const int kPortCount = 2;

/** Free space of an empty 8 MB card, in 1 KB clusters, as sceMcGetInfo reports it. */
const int kCardClusters = 8000;

const int kClusterSize = 1024;
const int kMaxNameLength = 31;
const int kMaxOpenFiles = 32;

/** A file in a card folder that marks the card unformatted; it is not on the card itself. */
const char kUnformattedMarker[] = ".unformatted";

/** sceMcSync's answers. */
const int kSyncIdle = -1;
const int kSyncFinished = 1;

/** Function numbers sceMcSync reports. */
enum Function {
    FUNC_GET_INFO = 0x01,
    FUNC_OPEN = 0x02,
    FUNC_CLOSE = 0x03,
    FUNC_SEEK = 0x04,
    FUNC_READ = 0x05,
    FUNC_WRITE = 0x06,
    FUNC_FLUSH = 0x0A,
    FUNC_MKDIR = 0x0B,
    FUNC_CHDIR = 0x0C,
    FUNC_GET_DIR = 0x0D,
    FUNC_DELETE = 0x0F,
    FUNC_FORMAT = 0x10,
    FUNC_UNFORMAT = 0x11,
    FUNC_RENAME = 0x13,
};

/** Results beyond the ones libmc.h names. */
const int kResultNotEmpty = -6;
const int kResultUpLimitHandle = -7;
const int kResultNoCard = -10;

/** sceMcOpen flags. */
const int kOpenWrite = 0x0002;
const int kOpenCreate = 0x0200;

/** sceMcTblGetDir attributes. */
const unsigned short kAttrFile = 0x8497;
const unsigned short kAttrDirectory = 0x8427;

/** The time stamp of a directory entry, kept in Japan time as the PS2 keeps it. */
struct McDateTime {
    unsigned char  reserved;
    unsigned char  second;
    unsigned char  minute;
    unsigned char  hour;
    unsigned char  day;
    unsigned char  month;
    unsigned short year;
};

/** One entry sceMcGetDir writes (sceMcTblGetDir). */
struct McDirEntry {
    McDateTime     created;
    McDateTime     modified;
    unsigned int   size;
    unsigned short attributes;
    unsigned short reserved1;
    unsigned int   reserved2;
    unsigned int   pda_application;
    unsigned char  name[32];
};

static_assert(sizeof(McDirEntry) == 0x40, "sceMcTblGetDir is 64 bytes");

struct OpenFile {
    int                   port;
    std::filesystem::path path;
    std::fstream          stream;
    int64_t               position;
    bool                  writable;
};

struct Card {
    std::string current = "/"; /**< Current directory, always absolute and ending in '/'. */
    bool        reported = false;
};

struct Cards {
    std::recursive_mutex      lock;
    Card                      cards[kPortCount];
    std::unique_ptr<OpenFile> files[kMaxOpenFiles];
    std::vector<McDirEntry>   listing;          /**< What the last sceMcGetDir found... */
    size_t                    listing_next = 0; /**< ...and how much of it was handed out. */
    bool                      pending = false;
    int                       pending_function = 0;
    int                       pending_result = 0;
};

Cards &GetCards() {
    static Cards cards;
    return cards;
}

/** Records a finished command's result for sceMcSync, and gives what the command call returns. */
int Finish(int function, int result) {
    Cards &cards = GetCards();
    cards.pending = true;
    cards.pending_function = function;
    cards.pending_result = result;
    return 0;
}

std::filesystem::path CardFolder(int port) {
    return pc::MemoryCardPath() / (port == 0 ? "slot1" : "slot2");
}

bool Inserted(int port) {
    if (port < 0 || port >= kPortCount) {
        return false;
    }

    std::error_code error;

    if (port == 0) {
        std::filesystem::create_directories(CardFolder(port), error);
    }

    return std::filesystem::is_directory(CardFolder(port), error);
}

bool Formatted(int port) {
    std::error_code error;
    return !std::filesystem::exists(CardFolder(port) / kUnformattedMarker, error);
}

/** The result a command on a port gives when the card cannot take commands, or 0 when it can. */
int CardProblem(int port) {
    if (!Inserted(port)) {
        return kResultNoCard;
    }

    return Formatted(port) ? 0 : sceMcResNoFormat;
}

/**
 * Turns a card path, absolute or relative to the port's current directory,
 * into a canonical absolute one ("/DIR/FILE"), or gives false when it
 * climbs above the root or a name is too long.
 */
bool CardPath(int port, const char *name, std::string *out) {
    std::string              full = name[0] == '/' ? std::string(name) : GetCards().cards[port].current + name;
    std::vector<std::string> parts;
    size_t                   start = 0;

    while (start <= full.size()) {
        size_t end = full.find('/', start);

        if (end == std::string::npos) {
            end = full.size();
        }

        std::string part = full.substr(start, end - start);
        start = end + 1;

        if (part.empty() || part == ".") {
            continue;
        }

        if (part == "..") {
            if (parts.empty()) {
                return false;
            }

            parts.pop_back();
            continue;
        }

        if ((int) part.size() > kMaxNameLength) {
            return false;
        }

        parts.push_back(part);
    }

    out->clear();

    for (const std::string &part : parts) {
        *out += '/';
        *out += part;
    }

    if (out->empty()) {
        *out = "/";
    }

    return true;
}

std::filesystem::path HostPath(int port, const std::string &card_path) {
    return CardFolder(port) / pc::PathFromUtf8(card_path.substr(1));
}

McDateTime DateTimeOf(const std::filesystem::path &path) {
    std::error_code                 error;
    std::filesystem::file_time_type written = std::filesystem::last_write_time(path, error);
    std::time_t                     seconds = std::time(NULL);

    if (!error) {
        auto since = written - std::filesystem::file_time_type::clock::now();
        seconds += (std::time_t) std::chrono::duration_cast<std::chrono::seconds>(since).count();
    }

    // Japan time: UTC+9, without daylight saving.
    seconds += 9 * 60 * 60;
    std::tm parts = {};
#ifdef _WIN32
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif

    McDateTime stamp = {};
    stamp.second = (unsigned char) parts.tm_sec;
    stamp.minute = (unsigned char) parts.tm_min;
    stamp.hour = (unsigned char) parts.tm_hour;
    stamp.day = (unsigned char) parts.tm_mday;
    stamp.month = (unsigned char) (parts.tm_mon + 1);
    stamp.year = (unsigned short) (parts.tm_year + 1900);
    return stamp;
}

McDirEntry EntryFor(const std::filesystem::path &path, const std::string &name) {
    McDirEntry      entry = {};
    std::error_code error;

    entry.created = entry.modified = DateTimeOf(path);

    if (std::filesystem::is_directory(path, error)) {
        entry.attributes = kAttrDirectory;
        // A directory's size is the number of its entries, "." and ".." included.
        unsigned int count = 2;

        for (std::filesystem::directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
            count++;
        }

        entry.size = count;
    } else {
        entry.attributes = kAttrFile;
        entry.size = (unsigned int) std::filesystem::file_size(path, error);
    }

    std::strncpy(reinterpret_cast<char *>(entry.name), name.c_str(), sizeof(entry.name) - 1);
    return entry;
}

/** Matches a name against a pattern of '*' (any run) and '?' (any one character). */
bool Matches(const char *pattern, const char *name) {
    if (*pattern == '\0') {
        return *name == '\0';
    }

    if (*pattern == '*') {
        for (const char *rest = name;; rest++) {
            if (Matches(pattern + 1, rest)) {
                return true;
            }

            if (*rest == '\0') {
                return false;
            }
        }
    }

    return *name != '\0' && (*pattern == '?' || *pattern == *name) && Matches(pattern + 1, name + 1);
}

int UsedClusters(int port) {
    std::error_code error;
    int64_t         used = 0;

    for (std::filesystem::recursive_directory_iterator it(CardFolder(port), error), end; !error && it != end;
         it.increment(error)) {
        if (it->path().filename() == kUnformattedMarker) {
            continue;
        }

        if (it->is_directory(error)) {
            used += 2;
        } else {
            used += (int64_t) ((it->file_size(error) + kClusterSize - 1) / kClusterSize);
        }
    }

    return (int) std::min<int64_t>(used, kCardClusters);
}

OpenFile *FileFor(int fd) {
    return fd >= 0 && fd < kMaxOpenFiles ? GetCards().files[fd].get() : NULL;
}

} // namespace

extern "C" {

int sceMcInit(void) {
    return sceMcIniSucceed;
}

int sceMcEnd(void) {
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);

    for (std::unique_ptr<OpenFile> &file : GetCards().files) {
        file.reset();
    }

    return 0;
}

int sceMcSync(int mode, int *cmd, int *result) {
    (void) mode;
    Cards                                &cards = GetCards();
    std::lock_guard<std::recursive_mutex> guard(cards.lock);

    if (!cards.pending) {
        return kSyncIdle;
    }

    if (cmd != NULL) {
        *cmd = cards.pending_function;
    }

    if (result != NULL) {
        *result = cards.pending_result;
    }

    cards.pending = false;
    return kSyncFinished;
}

int sceMcGetInfo(int port, int slot, int *type, int *free_size, int *formatted) {
    (void) slot;
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);

    if (!Inserted(port)) {
        if (port >= 0 && port < kPortCount) {
            GetCards().cards[port].reported = false;
        }

        *type = sceMcTypeNoCard;
        *free_size = 0;
        *formatted = 0;
        return Finish(FUNC_GET_INFO, kResultNoCard);
    }

    Card &card = GetCards().cards[port];
    *type = sceMcTypePS2;
    *formatted = Formatted(port) ? 1 : 0;
    *free_size = *formatted ? kCardClusters - UsedClusters(port) : 0;

    // The first look at a card reports it as newly inserted.
    int result = sceMcResSucceed;

    if (!*formatted) {
        result = sceMcResNoFormat;
    } else if (!card.reported) {
        result = sceMcResChangedCard;
    }

    card.reported = true;
    return Finish(FUNC_GET_INFO, result);
}

int sceMcOpen(int port, int slot, const char *name, int flag) {
    (void) slot;
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);
    std::string                           path;
    int                                   problem = CardProblem(port);

    if (problem != 0) {
        return Finish(FUNC_OPEN, problem);
    }

    if (!CardPath(port, name, &path) || path == "/") {
        return Finish(FUNC_OPEN, sceMcResNoEntry);
    }

    std::filesystem::path host = HostPath(port, path);
    std::error_code       error;

    if (std::filesystem::is_directory(host, error)) {
        return Finish(FUNC_OPEN, sceMcResDeniedPermit);
    }

    if (!std::filesystem::exists(host, error)) {
        if (!(flag & kOpenCreate) || !std::filesystem::is_directory(host.parent_path(), error)) {
            return Finish(FUNC_OPEN, sceMcResNoEntry);
        }

        std::ofstream create(host, std::ios::binary);

        if (!create) {
            return Finish(FUNC_OPEN, sceMcResFullDevice);
        }
    }

    int fd = 0;

    while (fd < kMaxOpenFiles && GetCards().files[fd] != nullptr) {
        fd++;
    }

    if (fd == kMaxOpenFiles) {
        return Finish(FUNC_OPEN, kResultUpLimitHandle);
    }

    std::unique_ptr<OpenFile> file(new OpenFile());
    file->port = port;
    file->path = host;
    file->position = 0;
    file->writable = (flag & kOpenWrite) != 0;
    file->stream.open(host, std::ios::binary | std::ios::in | (file->writable ? std::ios::out : std::ios::in));

    if (!file->stream) {
        return Finish(FUNC_OPEN, sceMcResDeniedPermit);
    }

    GetCards().files[fd] = std::move(file);
    return Finish(FUNC_OPEN, fd);
}

int sceMcClose(int fd) {
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);

    if (FileFor(fd) == NULL) {
        return Finish(FUNC_CLOSE, sceMcResDeniedPermit);
    }

    GetCards().files[fd].reset();
    return Finish(FUNC_CLOSE, sceMcResSucceed);
}

int sceMcSeek(int fd, int offset, int origin) {
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);
    OpenFile                             *file = FileFor(fd);

    if (file == NULL) {
        return Finish(FUNC_SEEK, sceMcResDeniedPermit);
    }

    std::error_code error;
    int64_t         size = (int64_t) std::filesystem::file_size(file->path, error);
    int64_t         position = origin == 0 ? offset : origin == 1 ? file->position + offset
                                                                  : size + offset;

    if (position < 0) {
        position = 0;
    }

    file->position = position;
    return Finish(FUNC_SEEK, (int) position);
}

int sceMcRead(int fd, void *buffer, int size) {
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);
    OpenFile                             *file = FileFor(fd);

    if (file == NULL || size < 0) {
        return Finish(FUNC_READ, sceMcResDeniedPermit);
    }

    file->stream.clear();
    file->stream.seekg(file->position);
    file->stream.read(static_cast<char *>(buffer), size);
    int count = (int) file->stream.gcount();
    file->position += count;
    return Finish(FUNC_READ, count);
}

int sceMcWrite(int fd, void *buffer, int size) {
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);
    OpenFile                             *file = FileFor(fd);

    if (file == NULL || !file->writable || size < 0) {
        return Finish(FUNC_WRITE, sceMcResDeniedPermit);
    }

    std::error_code error;
    int64_t         size_now = (int64_t) std::filesystem::file_size(file->path, error);
    int64_t         growth = file->position + size - size_now;

    if (growth > 0 && UsedClusters(file->port) + growth / kClusterSize >= kCardClusters) {
        return Finish(FUNC_WRITE, sceMcResFullDevice);
    }

    file->stream.clear();
    file->stream.seekp(file->position);
    file->stream.write(static_cast<const char *>(buffer), size);

    if (!file->stream) {
        return Finish(FUNC_WRITE, sceMcResFullDevice);
    }

    file->position += size;
    return Finish(FUNC_WRITE, size);
}

int sceMcFlush(int fd) {
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);
    OpenFile                             *file = FileFor(fd);

    if (file == NULL) {
        return Finish(FUNC_FLUSH, sceMcResDeniedPermit);
    }

    file->stream.flush();
    return Finish(FUNC_FLUSH, sceMcResSucceed);
}

int sceMcChdir(int port, int slot, const char *name, char *current) {
    (void) slot;
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);
    std::string                           path;
    int                                   problem = CardProblem(port);

    if (problem != 0) {
        return Finish(FUNC_CHDIR, problem);
    }

    Card &card = GetCards().cards[port];

    if (current != NULL) {
        // The directory left, as libmc writes it: without the trailing '/', except for the root.
        std::string previous = card.current.size() > 1 ? card.current.substr(0, card.current.size() - 1) : "/";
        std::strcpy(current, previous.c_str());
    }

    std::error_code error;

    if (!CardPath(port, name, &path) || !std::filesystem::is_directory(HostPath(port, path), error)) {
        return Finish(FUNC_CHDIR, sceMcResNoEntry);
    }

    card.current = path == "/" ? path : path + "/";
    return Finish(FUNC_CHDIR, sceMcResSucceed);
}

int sceMcMkdir(int port, int slot, const char *name) {
    (void) slot;
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);
    std::string                           path;
    int                                   problem = CardProblem(port);

    if (problem != 0) {
        return Finish(FUNC_MKDIR, problem);
    }

    std::error_code error;

    if (!CardPath(port, name, &path) || path == "/") {
        return Finish(FUNC_MKDIR, sceMcResNoEntry);
    }

    std::filesystem::path host = HostPath(port, path);

    // An existing entry is reported as -4, which the game takes as "already there".
    if (std::filesystem::exists(host, error) || !std::filesystem::is_directory(host.parent_path(), error)) {
        return Finish(FUNC_MKDIR, sceMcResNoEntry);
    }

    if (UsedClusters(port) + 2 > kCardClusters) {
        return Finish(FUNC_MKDIR, sceMcResFullDevice);
    }

    std::filesystem::create_directory(host, error);
    return Finish(FUNC_MKDIR, error ? sceMcResFullDevice : sceMcResSucceed);
}

int sceMcDelete(int port, int slot, char *name) {
    (void) slot;
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);
    std::string                           path;
    int                                   problem = CardProblem(port);

    if (problem != 0) {
        return Finish(FUNC_DELETE, problem);
    }

    std::error_code error;

    if (!CardPath(port, name, &path) || path == "/") {
        return Finish(FUNC_DELETE, sceMcResNoEntry);
    }

    std::filesystem::path host = HostPath(port, path);

    if (!std::filesystem::exists(host, error)) {
        return Finish(FUNC_DELETE, sceMcResNoEntry);
    }

    if (std::filesystem::is_directory(host, error) && !std::filesystem::is_empty(host, error)) {
        return Finish(FUNC_DELETE, kResultNotEmpty);
    }

    for (std::unique_ptr<OpenFile> &file : GetCards().files) {
        if (file != nullptr && file->path == host) {
            return Finish(FUNC_DELETE, sceMcResDeniedPermit);
        }
    }

    std::filesystem::remove(host, error);
    return Finish(FUNC_DELETE, error ? sceMcResDeniedPermit : sceMcResSucceed);
}

int sceMcRename(int port, int slot, char *old_name, char *new_name) {
    (void) slot;
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);
    std::string                           path;
    int                                   problem = CardProblem(port);

    if (problem != 0) {
        return Finish(FUNC_RENAME, problem);
    }

    std::error_code error;

    // The new name is a name alone: the entry stays in its directory.
    if (!CardPath(port, old_name, &path) || path == "/" || std::strchr(new_name, '/') != NULL ||
        new_name[0] == '\0' || (int) std::strlen(new_name) > kMaxNameLength) {
        return Finish(FUNC_RENAME, sceMcResNoEntry);
    }

    std::filesystem::path from = HostPath(port, path);
    std::filesystem::path to = from.parent_path() / pc::PathFromUtf8(new_name);

    if (!std::filesystem::exists(from, error)) {
        return Finish(FUNC_RENAME, sceMcResNoEntry);
    }

    if (std::filesystem::exists(to, error)) {
        return Finish(FUNC_RENAME, sceMcResFailReplace);
    }

    std::filesystem::rename(from, to, error);
    return Finish(FUNC_RENAME, error ? sceMcResDeniedPermit : sceMcResSucceed);
}

int sceMcGetDir(int port, int slot, const char *name, int mode, int count, MC_DIR_ENTRY *table) {
    (void) slot;
    Cards                                &cards = GetCards();
    std::lock_guard<std::recursive_mutex> guard(cards.lock);
    int                                   problem = CardProblem(port);

    if (problem != 0) {
        return Finish(FUNC_GET_DIR, problem);
    }

    // Mode 0 starts a listing; any other mode goes on with the last one.
    if (mode == 0) {
        cards.listing.clear();
        cards.listing_next = 0;

        std::string full = name;
        size_t      slash = full.rfind('/');
        std::string directory = slash == std::string::npos ? std::string() : full.substr(0, slash + 1);
        std::string pattern = slash == std::string::npos ? full : full.substr(slash + 1);
        std::string path;

        if (!CardPath(port, directory.empty() ? "." : directory.c_str(), &path)) {
            return Finish(FUNC_GET_DIR, 0);
        }

        std::filesystem::path host = HostPath(port, path);
        std::error_code       error;

        if (!std::filesystem::is_directory(host, error)) {
            return Finish(FUNC_GET_DIR, 0);
        }

        // Every directory but the root holds "." and "..".
        if (path != "/") {
            for (const char *dots : {".", ".."}) {
                if (Matches(pattern.c_str(), dots)) {
                    cards.listing.push_back(EntryFor(host, dots));
                }
            }
        }

        for (std::filesystem::directory_iterator it(host, error), end; !error && it != end; it.increment(error)) {
            std::string entry_name = pc::Utf8FromPath(it->path().filename());

            if (entry_name != kUnformattedMarker && Matches(pattern.c_str(), entry_name.c_str())) {
                cards.listing.push_back(EntryFor(it->path(), entry_name));
            }
        }
    }

    int written = 0;

    while (written < count && cards.listing_next < cards.listing.size()) {
        std::memcpy(reinterpret_cast<McDirEntry *>(table) + written, &cards.listing[cards.listing_next],
                    sizeof(McDirEntry));
        cards.listing_next++;
        written++;
    }

    return Finish(FUNC_GET_DIR, written);
}

int sceMcFormat(int port, int slot) {
    (void) slot;
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);

    if (!Inserted(port)) {
        return Finish(FUNC_FORMAT, kResultNoCard);
    }

    std::error_code error;

    for (std::unique_ptr<OpenFile> &file : GetCards().files) {
        if (file != nullptr && file->port == port) {
            file.reset();
        }
    }

    for (std::filesystem::directory_iterator it(CardFolder(port), error), end; !error && it != end;
         it.increment(error)) {
        std::error_code remove_error;
        std::filesystem::remove_all(it->path(), remove_error);
    }

    GetCards().cards[port].current = "/";
    return Finish(FUNC_FORMAT, sceMcResSucceed);
}

int sceMcUnformat(int port, int slot) {
    std::lock_guard<std::recursive_mutex> guard(GetCards().lock);
    sceMcFormat(port, slot);
    GetCards().pending = false;

    if (!Inserted(port)) {
        return Finish(FUNC_UNFORMAT, kResultNoCard);
    }

    std::ofstream marker(CardFolder(port) / kUnformattedMarker);
    return Finish(FUNC_UNFORMAT, sceMcResSucceed);
}

} // extern "C"
