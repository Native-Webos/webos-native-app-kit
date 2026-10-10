# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build webosbrew/SDL-webOS from a pinned tag with the same NDK as the app, and expose it as the
# INTERFACE target `wnk_sdl2`. ADR-003 in docs/research/ARCHITECTURE-DECISIONS.md explains why the kit
# bundles SDL instead of linking the TV's own (2.0.10 on webOS 6, 2.0.14 on 7-11).
#
# Configure flags are those of SDL-webOS's own webOS CI (.github/workflows/webos.yml) and of
# moonlight-tv's commons-c ExternalSDL2BackportForWebOS.cmake, with one deliberate choice:
# SDL_WEBOS_BROKEN_ABI=OFF, so SDL_Event has the standard upstream layout and host and TV code share
# one struct definition (the TV's system SDL inserts an extra `inputSource` field).
#
# The result is staged into the package as lib/libSDL2-2.0.so.0 and found through RPATH $ORIGIN/lib.

include(ExternalProject)

set(WNK_SDL_WEBOS_TAG "release-2.30.12-webos.6" CACHE STRING "webosbrew/SDL-webOS git tag")
set(WNK_SDL_WEBOS_REPO "https://github.com/webosbrew/SDL-webOS.git" CACHE STRING "SDL-webOS repository")

set(_wnk_sdl_prefix "${CMAKE_BINARY_DIR}/third_party/sdl-webos")
set(WNK_SDL_WEBOS_LIBRARY "${_wnk_sdl_prefix}/lib/libSDL2-2.0.so.0" CACHE INTERNAL "")

ExternalProject_Add(wnk_sdl_webos_ext
        GIT_REPOSITORY "${WNK_SDL_WEBOS_REPO}"
        GIT_TAG "${WNK_SDL_WEBOS_TAG}"
        GIT_SHALLOW ON
        GIT_PROGRESS OFF
        UPDATE_DISCONNECTED ON
        PREFIX "${CMAKE_BINARY_DIR}/third_party/sdl-webos-build"
        INSTALL_DIR "${_wnk_sdl_prefix}"
        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}"
            "-DCMAKE_BUILD_TYPE:STRING=Release"
            "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>"
            -DWEBOS=ON
            -DSDL_WEBOS_BROKEN_ABI=OFF
            -DSDL_OFFSCREEN=OFF -DSDL_DISKAUDIO=OFF -DSDL_DUMMYAUDIO=OFF -DSDL_DUMMYVIDEO=OFF
            -DSDL_KMSDRM=OFF -DSDL_STATIC=OFF -DSDL_TEST=OFF
            "-DSDL_VENDOR_INFO:STRING=webOS Backport (webos-native-app-kit)"
        BUILD_BYPRODUCTS "${WNK_SDL_WEBOS_LIBRARY}"
)

# The include directory must exist at configure time for INTERFACE_INCLUDE_DIRECTORIES.
file(MAKE_DIRECTORY "${_wnk_sdl_prefix}/include/SDL2")

add_library(wnk_sdl2_shared SHARED IMPORTED GLOBAL)
set_target_properties(wnk_sdl2_shared PROPERTIES
        IMPORTED_LOCATION "${WNK_SDL_WEBOS_LIBRARY}"
        IMPORTED_SONAME "libSDL2-2.0.so.0"
        INTERFACE_INCLUDE_DIRECTORIES "${_wnk_sdl_prefix}/include/SDL2")
add_dependencies(wnk_sdl2_shared wnk_sdl_webos_ext)

add_library(wnk_sdl2 INTERFACE)
target_link_libraries(wnk_sdl2 INTERFACE wnk_sdl2_shared)
target_compile_definitions(wnk_sdl2 INTERFACE WNK_SDL_WEBOS=1)

unset(_wnk_sdl_prefix)
