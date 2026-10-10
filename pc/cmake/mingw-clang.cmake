# Cross-compiles the PC build from Linux with Clang and the MinGW-w64
# headers and libraries (Debian/Ubuntu: apt install clang lld mingw-w64).
# The platform needs libstdc++'s threading, which only the "posix" variant
# of MinGW's GCC has; Clang takes whichever of /usr/lib/gcc/x86_64-w64-mingw32/
# 13-posix and 13-win32 sorts last, so move 13-win32 out of the way.
#
#   cmake -S . -B build/mingw -G Ninja -DPLATFORM=PC \
#       -DCMAKE_TOOLCHAIN_FILE=pc/cmake/mingw-clang.cmake
#
# The result runs under Wine, which is how the build is tried without Windows.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-w64-mingw32)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-mingw32)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -pthread")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
# MinGW's libstdc++ is built by GCC, whose thread-local variables are emulated.
set(CMAKE_C_FLAGS_INIT "-femulated-tls")
set(CMAKE_CXX_FLAGS_INIT "-femulated-tls")
