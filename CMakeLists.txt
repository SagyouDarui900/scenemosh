# SceneMosh plugin — core library (M1), tests, and the .aux2
# plugin DLL (M2 media object + M3 cut scan / auto-place panel).
#
#   Linux (this VM, no AviUtl2 / MSVC): core + tests only.
#     cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#     cmake --build build -j
#     ./build/test_core
#
#   Windows (MSVC + Ninja, see README_BUILD.md): core + tests + DLL.
#     cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build build
#
#   MinGW-w64 cross-compile check (this VM, DLL link check only):
#     cmake -S . -B build-mingw --toolchain cmake/toolchain-mingw64.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-mingw
#
# The core is platform-independent C++20 except for the #ifdef _WIN32
# branches in sha256.cpp (BCrypt), process.cpp (CreateProcess) and
# file_lock.cpp (sharing check). The plugin (src/plugin) is Windows-only
# and is only built when the target system is Windows.

cmake_minimum_required(VERSION 3.16)
project(scenemosh CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_library(hybrid_core STATIC
    src/core/cache_index.cpp
    src/core/cut_detector.cpp
    src/core/downloader.cpp
    src/core/encoder.cpp
    src/core/ffglitch.cpp
    src/core/file_lock.cpp
    src/core/frame_fit.cpp
    src/core/generator.cpp
    src/core/hash.cpp
    src/core/log.cpp
    src/core/mp4_exporter.cpp
    src/core/presets.cpp
    src/core/process.cpp
    src/core/settings.cpp
    src/core/sha256.cpp
)
target_include_directories(hybrid_core PUBLIC src/core)
if(WIN32)
    target_link_libraries(hybrid_core PRIVATE bcrypt wininet)
endif()

option(HYBRID_BUILD_TESTS "Build the doctest suite" ON)
if(HYBRID_BUILD_TESTS)
    add_executable(test_core tests/test_core.cpp)
    # src/plugin is only for the std-only marker_cache.hpp (F2 test);
    # the Windows-only plugin sources are never built here.
    target_include_directories(test_core PRIVATE vendor src/plugin)
    target_link_libraries(test_core PRIVATE hybrid_core)
    enable_testing()
    add_test(NAME core COMMAND test_core)
endif()

# The .aux2 plugin DLL (M2). Windows only: it uses the AviUtl2 SDK
# and Win32 APIs throughout.
option(HYBRID_BUILD_PLUGIN "Build the .aux2 plugin DLL" ON)
if(WIN32 AND HYBRID_BUILD_PLUGIN)
    add_library(scenemosh_aux2 SHARED
        src/plugin/context.cpp
        src/plugin/cut_scan.cpp
        src/plugin/items.cpp
        src/plugin/panel.cpp
        src/plugin/plugin.cpp
        src/plugin/render.cpp
        src/plugin/sdk_logger.cpp
        src/plugin/sdk_source_reader.cpp
        src/plugin/timeline_placer.cpp
        src/plugin/util.cpp
    )
    target_include_directories(scenemosh_aux2 PRIVATE
        src/plugin src/core vendor/aviutl2_sdk)
    # comctl32: the panel progress bar (PROGRESS_CLASS).
    target_link_libraries(scenemosh_aux2 PRIVATE hybrid_core user32 comctl32)
    target_compile_definitions(scenemosh_aux2 PRIVATE NOMINMAX)
    set_target_properties(scenemosh_aux2 PROPERTIES
        OUTPUT_NAME "scenemosh"
        PREFIX ""
        SUFFIX ".aux2")
    if(NOT MSVC)
        # Fully static MinGW runtime (libgcc, libstdc++, libwinpthread)
        # so the DLL has no MinGW runtime dependency on the user's PC.
        # Source files are UTF-8.
        target_link_options(scenemosh_aux2 PRIVATE -static)
        target_compile_options(scenemosh_aux2 PRIVATE
            -finput-charset=UTF-8 -fexec-charset=UTF-8)
    endif()
endif()
