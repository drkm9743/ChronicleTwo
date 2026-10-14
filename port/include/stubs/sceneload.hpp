#pragma once

#include <sceneload.hpp>

// Lets the seamless lakeside (port/src/town_seamless.cpp) load a map into a stack of its own.
uptr LoadMapData(SCN_LOADMAP_INFO2 &info, int deferred) asm("SceneLoad_LoadMapData");
