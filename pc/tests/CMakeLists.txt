# Tests of the platform's disc, memory card, SIF and sound code, built on the
# host (Linux, macOS or Windows) as a project of their own:
#
#   cmake -S pc/tests -B build/pc-tests -G Ninja -DCMAKE_PREFIX_PATH=<SDL 3.4.18>
#   cmake --build build/pc-tests && ctest --test-dir build/pc-tests
#
# They make their own disc, memory card and sound data in a temporary folder,
# so they need nothing from the real disc.

cmake_minimum_required(VERSION 3.20)
project(chronicletwo_platform_tests LANGUAGES CXX)

find_package(SDL3 3.4.18 EXACT CONFIG REQUIRED)

set(REPO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${REPO_DIR}/pc/src/platform_sources.cmake)

add_executable(platform_tests platform_tests.cpp ${PLATFORM_IO_SOURCES})
target_include_directories(platform_tests PRIVATE
    ${REPO_DIR}/pc/src
    ${REPO_DIR}/ps2/include/sce
    ${REPO_DIR}/ps2/include)
target_compile_definitions(platform_tests PRIVATE PLATFORM_PC)
target_link_libraries(platform_tests PRIVATE SDL3::SDL3)
set_target_properties(platform_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

enable_testing()
add_test(NAME platform_tests COMMAND platform_tests)
