# pc/gfx: the GS-level renderer of the PC build. Builds as part of
# PLATFORM=WIN64 (pc/CMakeLists.txt adds it) or on its own for tests:
#
#   cmake -S pc/gfx -B build/gfx -G Ninja && cmake --build build/gfx
#   ctest --test-dir build/gfx
#
# Targets:
#   chronicletwo_gfx_core     GS memory and registers, GIF, VIF1, VU1 host, DMA (no GPU)
#   chronicletwo_gfx_vulkan   the Vulkan renderer (Vulkan headers only; the loader comes at run time)
#   chronicletwo_gfx_sdl      window and swapchain on SDL 3 (needs the SDL3::SDL3 target)

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(chronicletwo_gfx CXX)
    set(GFX_STANDALONE ON)
else()
    set(GFX_STANDALONE OFF)
endif()

option(GFX_TESTS "Build the pc/gfx tests" ${GFX_STANDALONE})

add_library(chronicletwo_gfx_core STATIC
    gfx.cpp
    dma/dma.cpp
    gif/gif.cpp
    gs/gs_memory.cpp
    gs/gs_state.cpp
    vif/vif1.cpp
    vif/vu1_host.cpp)
target_include_directories(chronicletwo_gfx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(chronicletwo_gfx_core PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

#
# Vulkan headers: the Vulkan SDK's when CMake finds them, otherwise the copy
# SDL 3 ships (when SDL was built from source here), otherwise the system's.
#

find_package(Vulkan QUIET)
if(TARGET Vulkan::Headers)
    set(GFX_VULKAN_HEADERS Vulkan::Headers)
elseif(DEFINED GFX_SDL_SOURCE_DIR AND EXISTS ${GFX_SDL_SOURCE_DIR}/src/video/khronos/vulkan/vulkan.h)
    add_library(gfx_vulkan_headers INTERFACE)
    target_include_directories(gfx_vulkan_headers INTERFACE ${GFX_SDL_SOURCE_DIR}/src/video/khronos)
    set(GFX_VULKAN_HEADERS gfx_vulkan_headers)
else()
    find_path(GFX_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
    if(GFX_VULKAN_INCLUDE_DIR)
        add_library(gfx_vulkan_headers INTERFACE)
        target_include_directories(gfx_vulkan_headers INTERFACE ${GFX_VULKAN_INCLUDE_DIR})
        set(GFX_VULKAN_HEADERS gfx_vulkan_headers)
    endif()
endif()

if(GFX_VULKAN_HEADERS)
    add_library(chronicletwo_gfx_vulkan STATIC
        vulkan/vk_context.cpp
        vulkan/vk_functions.cpp
        vulkan/vk_renderer.cpp
        vulkan/vk_shaders.cpp)
    target_link_libraries(chronicletwo_gfx_vulkan PUBLIC chronicletwo_gfx_core ${GFX_VULKAN_HEADERS})
    # Functions are loaded through vkGetInstanceProcAddr; nothing links the loader.
    target_compile_definitions(chronicletwo_gfx_vulkan PUBLIC VK_NO_PROTOTYPES)
    if(WIN32)
        target_compile_definitions(chronicletwo_gfx_vulkan PUBLIC VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    set_target_properties(chronicletwo_gfx_vulkan PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(chronicletwo_gfx_vulkan PRIVATE ${CMAKE_DL_LIBS})

    if(GFX_STANDALONE AND NOT TARGET SDL3::SDL3)
        find_package(SDL3 CONFIG QUIET)
    endif()
    if(TARGET SDL3::SDL3)
        add_library(chronicletwo_gfx_sdl STATIC vulkan/vk_window_sdl.cpp)
        target_link_libraries(chronicletwo_gfx_sdl PUBLIC chronicletwo_gfx_vulkan SDL3::SDL3)
        set_target_properties(chronicletwo_gfx_sdl PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    endif()
else()
    message(STATUS "pc/gfx: no Vulkan headers found, building the core only")
endif()

#
# Shaders: SPIR-V is checked in as headers (shaders/*.spv.h) so a build needs
# no shader compiler. With glslangValidator present, the gfx_shaders target
# regenerates them from the GLSL.
#

find_program(GFX_GLSLANG glslangValidator)
if(GFX_GLSLANG)
    set(GFX_SHADER_OUTPUTS)
    foreach(stage vert frag)
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gs.${stage})
        set(out ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gs_${stage}.spv.h)
        add_custom_command(OUTPUT ${out}
            COMMAND ${GFX_GLSLANG} -V --target-env vulkan1.3 --vn kGs_${stage}_spv -o ${out} ${src}
            DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gs_common.glsl
            COMMENT "Compiling shaders/gs.${stage}")
        list(APPEND GFX_SHADER_OUTPUTS ${out})
    endforeach()
    add_custom_target(gfx_shaders DEPENDS ${GFX_SHADER_OUTPUTS})
endif()

if(GFX_TESTS)
    enable_testing()
    add_executable(gfx_core_test tests/gfx_core_test.cpp)
    target_link_libraries(gfx_core_test PRIVATE chronicletwo_gfx_core)
    set_target_properties(gfx_core_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME gfx_core_test COMMAND gfx_core_test)

    if(TARGET chronicletwo_gfx_vulkan)
        add_executable(gfx_vulkan_test tests/gfx_vulkan_test.cpp)
        target_link_libraries(gfx_vulkan_test PRIVATE chronicletwo_gfx_vulkan)
        set_target_properties(gfx_vulkan_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        # Needs a Vulkan driver; lavapipe is enough.
        add_test(NAME gfx_vulkan_test COMMAND gfx_vulkan_test)
    endif()
    if(TARGET chronicletwo_gfx_sdl)
        # Not a test: opens a window (run it by hand to check presentation).
        add_executable(gfx_sdl_demo tests/gfx_sdl_demo.cpp)
        target_link_libraries(gfx_sdl_demo PRIVATE chronicletwo_gfx_sdl)
        set_target_properties(gfx_sdl_demo PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    endif()
endif()
