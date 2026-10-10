# The 64-bit Windows build: compiles every game unit in ps2/src with Clang
# into a static library. Nothing here touches the PS2 build; the sources see
# PLATFORM_PC and NONMATCHING, which MWCC never defines.
#
# Configure with clang-cl (or clang++ targeting x86_64-pc-windows-msvc):
#   cmake -S . -B build/win64 -G Ninja -DPLATFORM=WIN64 -DCMAKE_CXX_COMPILER=clang-cl
#   cmake --build build/win64
#
# Targets:
#   chronicletwo_game (default)   every game unit, as a static library
#
# The PS2 SDK and the Metrowerks runtime the units call are not implemented
# yet, so nothing links against the library so far.

enable_language(CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "PLATFORM=WIN64 needs Clang (clang-cl or clang++), not "
        "${CMAKE_CXX_COMPILER_ID}: the sources use GNU attributes and 128-bit "
        "integers that MSVC does not accept.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "PLATFORM=WIN64 needs a 64-bit target.")
endif()

option(PC_LAYOUT_ASSERTS
    "Check the PS2 structure sizes (STATIC_ASSERT); many differ with 8-byte pointers"
    OFF)

set(GAME_SRC_DIR ${CMAKE_SOURCE_DIR}/ps2/src)
set(GAME_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/ps2/include)

# scripts/build/globs.sh keeps the PS2 build's list; here every unit is built.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${GAME_SRC_DIR}/*.cpp)

add_library(chronicletwo_game STATIC ${GAME_SOURCES})

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
