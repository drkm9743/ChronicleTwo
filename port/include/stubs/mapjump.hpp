#pragma once

#include <mapjump.hpp>

// Expose the retail sub map slot settings to the seamless lakeside (port/src/town_seamless.cpp).
extern MapJumpMapInfo SubMapInfo asm("MapJump_SubMapInfo");
extern int            NowSubMapNo asm("MapJump_NowSubMapNo");
