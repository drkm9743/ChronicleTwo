cmake_minimum_required(VERSION 3.20)
project(chronicletwo NONE)

# PLATFORM picks what is built: PS2 builds and verifies the retail executable
# (ps2/CMakeLists.txt); WIN64 compiles the same game sources for 64-bit
# Windows with Clang (pc/CMakeLists.txt).
set(PLATFORM PS2 CACHE STRING "Platform to build")
set_property(CACHE PLATFORM PROPERTY STRINGS PS2 WIN64)
if(PLATFORM STREQUAL "PS2")
    add_subdirectory(ps2)
elseif(PLATFORM STREQUAL "WIN64")
    add_subdirectory(pc)
else()
    message(FATAL_ERROR "PLATFORM must be PS2 or WIN64, not ${PLATFORM}")
endif()
