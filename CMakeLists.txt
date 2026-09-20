cmake_minimum_required(VERSION 3.16)
# C is here for src/shared/, which is plain C so that cod1plus.so (gcc, no C++) can
# compile the very same file. See src/shared/lean_controllers.h.
project(cod1reloaded LANGUAGES C CXX)

# CoD1 is a 32-bit game. Force 32-bit build.
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "cod1reloaded must be built as 32-bit. "
        "Use a 32-bit MinGW (e.g. i686-w64-mingw32-g++) or pass -m32.")
endif()

# Native multilib gcc (msys2) needs -m32 to force 32-bit. The i686-w64-mingw32
# cross-compiler is already 32-bit and rejects -m32, so only add it natively.
if (NOT CMAKE_CROSSCOMPILING)
    add_compile_options(-m32)
    add_link_options(-m32)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(COD1RELOADED_SOURCES
    # Compiled into cod1plus.so as well, from a byte-identical copy in that repo.
    # If you change it here, copy it there (and vice versa) - both sides print
    # lc_banner() at install so a drift shows up in the two logs.
    src/shared/lean_controllers.c
    src/core/main.cpp
    src/core/patches.cpp
    src/gameplay/viewheight_fix.cpp
    src/gameplay/lean_fix.cpp
    src/gameplay/swing_fix.cpp
    src/core/logger.cpp
    src/core/iat.cpp
    src/input/rinput.cpp
    src/netcode/version_patch.cpp
    src/netcode/protocol_patch.cpp
    src/netcode/competitive.cpp
    src/netcode/cheat_scan.cpp
    src/netcode/ruleset_eval.cpp
    src/netcode/ruleset_parse.cpp
    src/netcode/ruleset_fetch.cpp
    src/netcode/ruleset.cpp
    src/netcode/version_gate.cpp
    src/video/window_patch.cpp
    src/video/fullscreen_patch.cpp
    src/video/display_probe.cpp
    src/performance/fps_cap.cpp
    src/performance/frame_limiter.cpp
    src/features/updater.cpp
    src/features/demo_upload.cpp
    src/core/toast.cpp
    src/performance/cpu_affinity.cpp
    src/performance/process_priority.cpp
    src/performance/working_set.cpp
    src/performance/fso_disable.cpp
    src/video/widescreen_fix.cpp
    src/video/gamma_fix.cpp
    src/features/avatar_overlay.cpp
    src/features/engine_2d.cpp
    src/features/discord_rpc.cpp
    src/features/settings_menu.cpp
    src/gameplay/stance_fix.cpp
    src/gameplay/anim_clamp.cpp
    src/netcode/antilag.cpp
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

# Build a DLL that never touches the mouse at all, for bisecting input problems.
# `raw_mouse_input = off` in the ini is NOT the same thing: it only sets the default of
# the m_rinput cvar, while rinput_start() still installs the GetCursorPos IAT hook at
# DllMain. With this option the hook is never installed and the engine's mouse path is
# exactly vanilla.
#     cmake -S . -B build-norinput -DCOD1RELOADED_NO_RINPUT=ON ...
option(COD1RELOADED_NO_RINPUT "Do not install the raw-input GetCursorPos hook" OFF)

# The two variants MUST NOT share an output directory. Both build trees resolve their
# output against CMAKE_SOURCE_DIR, so before this they both wrote <source>/build/mss32.dll
# and whichever was built last silently replaced the other - a release could ship the
# wrong one with nothing to show for it. The DLL cannot be renamed (it is a proxy and has
# to be called mss32.dll), so the variants are separated by DIRECTORY instead.
set(COD1RELOADED_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build")
if (COD1RELOADED_NO_RINPUT)
    target_compile_definitions(cod1reloaded PRIVATE COD1RELOADED_NO_RINPUT=1)
    target_compile_definitions(mss32 PRIVATE COD1RELOADED_NO_RINPUT=1)
    set(COD1RELOADED_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build/norinput")
    message(STATUS "cod1reloaded: raw input DISABLED at compile time (no GetCursorPos hook)")
    message(STATUS "cod1reloaded: output -> ${COD1RELOADED_OUTPUT_DIR}")
endif()

target_compile_options(cod1reloaded PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -O2
)
target_compile_options(mss32 PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -O2
)

target_link_options(cod1reloaded PRIVATE
    -static -static-libgcc -static-libstdc++
    # -static embeds libwinpthread so the DLL has zero non-Windows deps.
    # No --kill-at: we want plain symbol names for now.
)
target_link_options(mss32 PRIVATE
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
    RUNTIME_OUTPUT_DIRECTORY "${COD1RELOADED_OUTPUT_DIR}"
)

set_target_properties(mss32 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mss32"
    RUNTIME_OUTPUT_DIRECTORY "${COD1RELOADED_OUTPUT_DIR}"
)
