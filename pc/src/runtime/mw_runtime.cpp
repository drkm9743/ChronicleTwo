// What the game links from the Metrowerks runtime and from newlib on the PS2,
// and the host's C library does not have under those names.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// newlib's per-thread state, as far as the game's <cstdio> reaches into it:
// stdin, stdout and stderr through _impure_ptr[0].
struct Reent {
    int   errno_value;
    FILE *in;
    FILE *out;
    FILE *err;
};

Reent reent;

} // namespace

extern "C" {

Reent *_impure_ptr[1] = {&reent};

void mwInit() {
    reent.in = stdin;
    reent.out = stdout;
    reent.err = stderr;
}

int fptosi(float value) {
    // cvt.w.s: truncation, and the largest int for what does not fit.
    if (value != value || value >= 2147483648.0f) {
        return 0x7FFFFFFF;
    }
    if (value < -2147483648.0f) {
        return int(0x80000000u);
    }
    return int(value);
}

unsigned int fptoui(float value) {
    if (value != value || value <= 0.0f) {
        return value <= -1.0f ? unsigned(fptosi(value)) : 0u;
    }
    if (value >= 4294967296.0f) {
        return 0xFFFFFFFFu;
    }
    if (value >= 2147483648.0f) {
        return unsigned(int(value - 2147483648.0f)) + 0x80000000u;
    }
    return unsigned(int(value));
}

/**
 * new[] of a class with a constructor or destructor: the block starts with
 * a 16-byte header holding the element size and count, then the elements.
 */
void *__construct_new_array(void *block, void *(*constructor)(void *), void *destructor, unsigned int element_size,
                            int count) {
    (void) destructor;
    if (block == nullptr) {
        return nullptr;
    }
    unsigned int *header = static_cast<unsigned int *>(block);
    header[0] = element_size;
    header[1] = unsigned(count);
    char *elements = static_cast<char *>(block) + 0x10;
    if (constructor != nullptr) {
        for (int i = 0; i < count; i++) {
            constructor(elements + size_t(i) * element_size);
        }
    }
    return elements;
}

char *index(const char *s, int c) { return const_cast<char *>(std::strchr(s, c)); }

#ifndef __MINGW32__ // MinGW's <string.h> has it
int strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = std::tolower(static_cast<unsigned char>(*a));
        int cb = std::tolower(static_cast<unsigned char>(*b));
        if (ca != cb || ca == 0) {
            return ca - cb;
        }
    }
}
#endif

} // extern "C"
