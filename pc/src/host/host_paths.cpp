#include "host_paths.h"

#include <SDL3/SDL.h>

#include <mutex>
#include <system_error>

namespace pc {

namespace {

struct Paths {
    std::mutex            lock;
    std::filesystem::path disc;
    std::filesystem::path host;
    std::filesystem::path memory_card;
};

Paths &GetPaths() {
    static Paths paths;
    return paths;
}

/** The folder holding the executable, or the working folder when SDL cannot tell. */
std::filesystem::path BaseFolder() {
    const char *base = SDL_GetBasePath();
    return base != NULL ? PathFromUtf8(base) : std::filesystem::current_path();
}

std::filesystem::path FromEnvironment(const char *name) {
    const char *value = SDL_getenv(name);
    return value != NULL && value[0] != '\0' ? PathFromUtf8(value) : std::filesystem::path();
}

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char) (c - 'A' + 'a') : c;
}

} // namespace

std::filesystem::path PathFromUtf8(const std::string &text) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
#else
    return std::filesystem::u8path(text);
#endif
}

std::string Utf8FromPath(const std::filesystem::path &path) {
    auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path DiscPath() {
    Paths                      &paths = GetPaths();
    std::lock_guard<std::mutex> guard(paths.lock);

    if (paths.disc.empty()) {
        paths.disc = FromEnvironment("DC2_DISC");
    }

    if (paths.disc.empty()) {
        paths.disc = BaseFolder() / "disc";
    }

    return paths.disc;
}

std::filesystem::path HostPath() {
    Paths                      &paths = GetPaths();
    std::lock_guard<std::mutex> guard(paths.lock);

    if (paths.host.empty()) {
        paths.host = FromEnvironment("DC2_HOST");
    }

    if (paths.host.empty()) {
        paths.host = BaseFolder() / "host";
    }

    return paths.host;
}

std::filesystem::path MemoryCardPath() {
    Paths                      &paths = GetPaths();
    std::lock_guard<std::mutex> guard(paths.lock);

    if (paths.memory_card.empty()) {
        paths.memory_card = FromEnvironment("DC2_MEMCARD");
    }

    if (paths.memory_card.empty()) {
        char *pref = SDL_GetPrefPath("ChronicleTwo", "Dark Chronicle");

        if (pref != NULL) {
            paths.memory_card = PathFromUtf8(pref) / "memcard";
            SDL_free(pref);
        } else {
            paths.memory_card = BaseFolder() / "memcard";
        }
    }

    return paths.memory_card;
}

void SetDiscPath(const std::filesystem::path &path) {
    std::lock_guard<std::mutex> guard(GetPaths().lock);
    GetPaths().disc = path;
}

void SetHostPath(const std::filesystem::path &path) {
    std::lock_guard<std::mutex> guard(GetPaths().lock);
    GetPaths().host = path;
}

void SetMemoryCardPath(const std::filesystem::path &path) {
    std::lock_guard<std::mutex> guard(GetPaths().lock);
    GetPaths().memory_card = path;
}

bool NamesEqualNoCase(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); i++) {
        if (Lower(a[i]) != Lower(b[i])) {
            return false;
        }
    }

    return true;
}

bool ResolveNoCase(const std::filesystem::path &root, const std::string &relative,
                   std::filesystem::path *out) {
    std::filesystem::path current = root;
    bool                  found = true;
    size_t                start = 0;

    while (start <= relative.size()) {
        size_t end = relative.find_first_of("/\\", start);

        if (end == std::string::npos) {
            end = relative.size();
        }

        std::string component = relative.substr(start, end - start);
        start = end + 1;

        if (component.empty() || component == ".") {
            continue;
        }

        if (component == "..") {
            current = current.parent_path();
            continue;
        }

        std::filesystem::path exact = current / PathFromUtf8(component);
        std::error_code       error;

        if (!found || std::filesystem::exists(exact, error)) {
            current = exact;
            continue;
        }

        bool matched = false;

        for (std::filesystem::directory_iterator it(current, error), end_it; !error && it != end_it;
             it.increment(error)) {
            if (NamesEqualNoCase(Utf8FromPath(it->path().filename()), component)) {
                current = it->path();
                matched = true;
                break;
            }
        }

        if (!matched) {
            current = exact;
            found = false;
        }
    }

    *out = current;
    return found;
}

} // namespace pc
