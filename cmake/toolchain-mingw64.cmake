# Cross-compile toolchain for Windows (x86_64) with MinGW-w64.
#
#   cmake -S . -B build-mingw --toolchain cmake/toolchain-mingw64.cmake \
#       -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-mingw
#
# Produces build-mingw/scenemosh.aux2. This is a compile/link
# check only: the DLL cannot run on this Linux VM (no AviUtl2).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
