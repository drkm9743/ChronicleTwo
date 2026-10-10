#ifndef COMMON_H
#define COMMON_H

/**
 * @file
 * Provides the integer aliases, assembly inclusion markers and size assertion used by game code.
 */

#include "include_asm.h"
#include "types.h"

#ifdef PLATFORM_PC
/*
 * The EE's memory-mapped registers (timers, DMAC, INTC, the GS privileged
 * registers) have nothing behind them on the PC. Where the game reads or
 * writes one directly, the PC build calls these with the EE address instead.
 */
#ifdef __cplusplus
extern "C" {
#endif
u_int  pcHwRead32(u_int address);
void   pcHwWrite32(u_int address, u_int value);
u_long pcHwRead64(u_int address);
void   pcHwWrite64(u_int address, u_long value);
#ifdef __cplusplus
}
#include "pc_ptr32.h"
#endif
#endif

/**
 * Declares a local whose constructor does not run, the job retail gives an
 * anonymous union around it. C++11 deletes such a union's constructor, so a
 * PC build declares raw storage and a reference to it instead.
 */
#ifdef PLATFORM_PC
#define PC_UNCONSTRUCTED(TYPE, NAME)                                  \
    alignas(TYPE) unsigned char NAME##_storage_[sizeof(TYPE)];        \
    TYPE &NAME = *reinterpret_cast<TYPE *>(NAME##_storage_)
#endif

/**
 * Checks a compile-time condition by forming an array type.
 *
 * The conditions are PS2 structure sizes. A 64-bit PC build lays many of them
 * out differently (pointers are 8 bytes), so there they are checked only when
 * PC_LAYOUT_ASSERTS is defined.
 */
#if !defined(PLATFORM_PC)
#define STATIC_ASSERT(expr) typedef char _static_assert_##__COUNTER__[(expr) ? 1 : -1]
#elif defined(PC_LAYOUT_ASSERTS)
#define STATIC_ASSERT(expr) static_assert(expr, #expr)
#else
#define STATIC_ASSERT(expr) typedef int _static_assert_skipped
#endif

#endif
