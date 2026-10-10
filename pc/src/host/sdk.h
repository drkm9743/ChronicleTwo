#pragma once

// The PS2 SDK headers whose functions the platform implements, for host code
// to include. Including them checks each definition against the declaration
// the game compiles with.
//
// Their types.h defines NULL as 0 and u_long as a 64-bit integer, which the
// host's headers define their own way (glibc's u_long is the C long), so the
// host's NULL is kept and the SDK's u_long is renamed.

#pragma push_macro("NULL")
#pragma push_macro("u_long")
#undef NULL
#define u_long ps2_u_long

#include <libcdvd.h>
#include <libmc.h>
#include <libsdr.h>
#include <modmsin.h>
#include <sifdev.h>
#include <sifdma.h>
#include <sifrpc.h>

#undef NULL
#undef u_long
#pragma pop_macro("u_long")
#pragma pop_macro("NULL")
