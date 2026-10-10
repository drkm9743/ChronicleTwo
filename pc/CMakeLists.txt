# The 64-bit Windows build: compiles every game unit in ps2/src with Clang
# into a static library. Nothing here touches the PS2 build; the sources see
# PLATFORM_PC and NONMATCHING, which MWCC never defines.
#
# Configure with clang-cl (or clang++ targeting x86_64-pc-windows-msvc):
#   cmake -S . -B build/win64 -G Ninja -DPLATFORM=PC -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#   cmake --build build/win64
# or cross-compile from Linux with LLVM MinGW (pc/cmake/mingw-clang.cmake).
#
# Targets:
#   chronicletwo (default)            the game, chronicletwo.exe
#   chronicletwo_game (default)       every game unit, as a static library
#   chronicletwo_platform (default)   the PS2 SDK libraries implemented on SDL 3
#                                     and the renderer in gfx/
#
# Run it with the PAL disc: chronicletwo.exe <ISO or extracted folder>.
#
# SDL is an installed SDL3 of exactly SDL_VERSION when CMake finds one, and
# otherwise built from its release, downloaded at configure time.

enable_language(CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "PLATFORM=PC needs Clang (clang-cl or clang++), not "
        "${CMAKE_CXX_COMPILER_ID}: the sources use GNU attributes and 128-bit "
        "integers that MSVC does not accept.")
endif()
if(NOT WIN32)
    message(FATAL_ERROR "PLATFORM=PC builds for Windows only for now (the EE kernel runs on Windows fibers).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "PLATFORM=PC needs a 64-bit target.")
endif()

option(PC_LAYOUT_ASSERTS
    "Check the PS2 structure sizes (STATIC_ASSERT); many differ with 8-byte pointers"
    OFF)

set(GAME_SRC_DIR ${CMAKE_SOURCE_DIR}/ps2/src)
set(GAME_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/ps2/include)

# scripts/build/globs.sh keeps the PS2 build's list; here every unit is built.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${GAME_SRC_DIR}/*.cpp)

# Plus what the PS2 build takes from retail bytes, read from the disc here.
add_library(chronicletwo_game STATIC ${GAME_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/retail_data.cpp)

# The game's own <cstring>, <libvu0.h> and so on stand in for the host's
# headers, as they do for MWCC, so the host's include paths are left out.
target_include_directories(chronicletwo_game PRIVATE
    ${GAME_INCLUDE_DIR}
    ${GAME_INCLUDE_DIR}/std
    ${GAME_INCLUDE_DIR}/sce)

target_compile_definitions(chronicletwo_game PRIVATE
    PLATFORM_PC
    NONMATCHING
    $<$<BOOL:${PC_LAYOUT_ASSERTS}>:PC_LAYOUT_ASSERTS>)

# C++14 with extensions: anonymous unions with constructors need C++11, and
# clang-cl starts at C++14.
set_target_properties(chronicletwo_game PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON)

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl: no host include paths, no exceptions or RTTI, and GNU keywords
    # (`typeof`) as MWCC has them.
    target_compile_options(chronicletwo_game PRIVATE
        /X /GR- /EHs-c- /w
        /clang:-nostdinc++ /clang:-fgnu-keywords
        -Wno-c++11-narrowing)
else()
    target_compile_options(chronicletwo_game PRIVATE
        -nostdinc -nostdinc++ -fno-exceptions -fno-rtti -fms-extensions -w
        -Wno-c++11-narrowing)
endif()

#
# Platform: the PS2 SDK on SDL 3
#

set(SDL_VERSION 3.4.18)
find_package(SDL3 ${SDL_VERSION} EXACT CONFIG QUIET)
if(SDL3_FOUND)
    message(STATUS "Using the installed SDL ${SDL3_VERSION}")
else()
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL3-${SDL_VERSION}.tar.gz
        URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL3)
endif()

# The renderer: the GS, GIF, VIF1 and DMA emulated in gfx/, drawn with Vulkan.
# SDL's source tree carries the Vulkan headers when no Vulkan SDK is installed.
if(DEFINED sdl3_SOURCE_DIR)
    set(GFX_SDL_SOURCE_DIR ${sdl3_SOURCE_DIR})
endif()
# The game's VU1 microprograms in C++ (pc/vu1), built by gfx/ with its adapter.
set(GFX_VU1_PROGRAMS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vu1 CACHE PATH "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/gfx ${CMAKE_CURRENT_BINARY_DIR}/gfx)
if(NOT TARGET chronicletwo_gfx_sdl)
    message(FATAL_ERROR "pc/gfx found no Vulkan headers: install the Vulkan SDK")
endif()

add_library(chronicletwo_platform STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ee/hardware.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ee/kernel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/pc_graphics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/mw_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sce/libdev_libmpeg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sce/libdma.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sce/libgraph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sce/libpad.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sce/libpkt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sce/libvu0.cpp)

# The disc, the memory cards, SIF and sound (pc/src/README.md).
include(${CMAKE_CURRENT_SOURCE_DIR}/src/platform_sources.cmake)
target_sources(chronicletwo_platform PRIVATE ${PLATFORM_IO_SOURCES})

# The platform code is ordinary host code: it sees the host's headers, and the
# SDK headers only for the declarations it implements (those pull in the
# game's common.h, so PLATFORM_PC picks the 64-bit spellings there).
target_include_directories(chronicletwo_platform PRIVATE ${GAME_INCLUDE_DIR}/sce)
target_include_directories(chronicletwo_platform PRIVATE ${GAME_INCLUDE_DIR})
target_compile_definitions(chronicletwo_platform PRIVATE PLATFORM_PC)
target_link_libraries(chronicletwo_platform PUBLIC SDL3::SDL3 chronicletwo_gfx_sdl chronicletwo_gfx_vu1 winmm)
set_target_properties(chronicletwo_platform PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

# Not built by default: opens a window (SDL_VIDEO_DRIVER=offscreen avoids it).
add_executable(chronicletwo_platform_gfx_test EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/platform_gfx_test.cpp)
target_include_directories(chronicletwo_platform_gfx_test PRIVATE ${GAME_INCLUDE_DIR}/sce ${GAME_INCLUDE_DIR})
target_compile_definitions(chronicletwo_platform_gfx_test PRIVATE PLATFORM_PC)
target_link_libraries(chronicletwo_platform_gfx_test PRIVATE chronicletwo_platform)
set_target_properties(chronicletwo_platform_gfx_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

#
# The executable
#

add_executable(chronicletwo ${CMAKE_CURRENT_SOURCE_DIR}/src/main/pc_main.cpp)
target_include_directories(chronicletwo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gfx)
target_link_libraries(chronicletwo PRIVATE chronicletwo_game chronicletwo_platform)
set_target_properties(chronicletwo PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

# The game keeps pointers in 32-bit integers (about 1,300 casts), so every
# address it can reach must be below 2 GiB: no large-address awareness, a low
# image base and no ASLR. Windows then keeps the whole process under 2 GiB.
# The scratchpad is mapped at 0x70000000, where the game writes it.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR MSVC)
    target_link_options(chronicletwo PRIVATE
        /LARGEADDRESSAWARE:NO /BASE:0x400000 /DYNAMICBASE:NO /HIGHENTROPYVA:NO /STACK:0x800000)
    # The game calls printf, sprintf and vsprintf as functions, which the UCRT
    # only exports through this library.
    target_link_libraries(chronicletwo PRIVATE legacy_stdio_definitions)
else()
    target_link_options(chronicletwo PRIVATE
        -Wl,-Xlink=-largeaddressaware:no -Wl,--image-base=0x400000
        -Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va -Wl,--stack=0x800000
        -static)
endif()
