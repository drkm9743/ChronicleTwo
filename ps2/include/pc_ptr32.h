#ifndef PC_PTR32_H
#define PC_PTR32_H

// PC only (common.h includes it under PLATFORM_PC). It is a header of its own
// because MWCC's output for some units shifts with the size of the headers it
// reads, even text an #ifdef skips. types.h gives it uintptr_t.

/**
 * A pointer kept in 4 bytes, as the EE keeps every pointer, for structures
 * the game reads from the disc or a memory card and uses in place: their
 * pointer fields hold offsets until the game turns them into addresses, and
 * the file's layout has room for 32 bits. The PC build keeps all its data
 * below 2 GiB (/LARGEADDRESSAWARE:NO), so an address fits.
 *
 * A field takes it under #ifdef PLATFORM_PC, with the PS2 declaration left
 * as it was in the #else.
 */
template <class T>
class ptr32 {
public:
    ptr32() = default;
    ptr32(T *pointer) : value_((u_int) (uintptr_t) pointer) {}
    ptr32 &operator=(T *pointer) {
        value_ = (u_int) (uintptr_t) pointer;
        return *this;
    }
    operator T *() const { return (T *) (uintptr_t) value_; }
    T *operator->() const { return (T *) (uintptr_t) value_; }
    template <class N>
    ptr32 &operator+=(N count) {
        value_ += (u_int) ((intptr_t) count * (intptr_t) sizeof(T));
        return *this;
    }
    template <class N>
    ptr32 &operator-=(N count) {
        value_ -= (u_int) ((intptr_t) count * (intptr_t) sizeof(T));
        return *this;
    }
    ptr32 &operator++() { return *this += 1; }
    ptr32 &operator--() { return *this -= 1; }
    T     *operator++(int) {
        T *old = *this;
        *this += 1;
        return old;
    }

private:
    u_int value_;
};

#endif
