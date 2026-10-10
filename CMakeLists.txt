cmake_minimum_required(VERSION 3.20)
project(chronicletwo NONE)

# PLATFORM picks what is built: PS2 builds and verifies the retail executable
# (ps2/CMakeLists.txt); PC compiles the same game sources for 64-bit
# Windows with Clang (pc/CMakeLists.txt).
set(PLATFORM PS2 CACHE STRING "Platform to build")
set_property(CACHE PLATFORM PROPERTY STRINGS PS2 PC)
# WIN64 was the PC build's first name.
if(PLATFORM STREQUAL "WIN64")
    set(PLATFORM PC CACHE STRING "Platform to build" FORCE)
endif()
if(PLATFORM STREQUAL "PS2")
    add_subdirectory(ps2)
elseif(PLATFORM STREQUAL "PC")
    add_subdirectory(pc)
else()
    message(FATAL_ERROR "PLATFORM must be PS2 or PC, not ${PLATFORM}")
endif()
