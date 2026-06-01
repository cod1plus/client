cmake_minimum_required(VERSION 3.16)
project(cod1reloaded LANGUAGES CXX)

# CoD1 is a 32-bit game. Force 32-bit build.
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "cod1reloaded must be built as 32-bit. "
        "Use a 32-bit MinGW (e.g. i686-w64-mingw32-g++) or pass -m32.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COD1RELOADED_SOURCES
    src/main.cpp
    src/patches.cpp
    src/viewheight_fix.cpp
    src/lean_fix.cpp
    src/swing_fix.cpp
    src/lean_amplify.cpp
    src/logger.cpp
    src/version_patch.cpp
    src/window_patch.cpp
    src/fullscreen_patch.cpp
    src/fps_cap.cpp
    src/frame_limiter.cpp
    src/updater.cpp
    src/demo_upload.cpp
    src/toast.cpp
    src/cpu_affinity.cpp
    src/process_priority.cpp
    src/working_set.cpp
    src/fso_disable.cpp
    src/widescreen_fix.cpp
    src/avatar_overlay.cpp
    src/engine_2d.cpp
)

add_library(cod1reloaded SHARED
    ${COD1RELOADED_SOURCES}
)

add_library(mss32 SHARED
    ${COD1RELOADED_SOURCES}
    src/mss32_proxy.def
)

target_include_directories(cod1reloaded PRIVATE src)
target_include_directories(mss32 PRIVATE src)

target_compile_options(cod1reloaded PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -m32 -O2
)
target_compile_options(mss32 PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -m32 -O2
)

target_link_options(cod1reloaded PRIVATE
    -m32
    -static -static-libgcc -static-libstdc++
    # -static embeds libwinpthread so the DLL has zero non-Windows deps.
    # No --kill-at: we want plain symbol names for now.
)
target_link_options(mss32 PRIVATE
    -m32
    -static -static-libgcc -static-libstdc++
)

target_link_libraries(cod1reloaded PRIVATE
    kernel32 user32 winmm wininet shell32 advapi32 gdiplus
)
target_link_libraries(mss32 PRIVATE
    kernel32 user32 winmm wininet shell32 advapi32 gdiplus
)

set_target_properties(cod1reloaded PROPERTIES
    PREFIX ""
    OUTPUT_NAME "cod1reloaded"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build"
)

set_target_properties(mss32 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mss32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build"
)
