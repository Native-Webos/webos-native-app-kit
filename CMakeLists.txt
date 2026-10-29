# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)

project(webos-native-app-kit VERSION 0.1.0 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(WebOSTarget)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WNK_BUILD_TESTS "Build host unit tests" ${WNK_HOST})
option(WNK_BUILD_EXAMPLES "Build example applications" ON)
set(WNK_SOURCE_URL "" CACHE STRING "Where packages' corresponding source is published (licenses/SOURCE.txt)")

if (WNK_WEBOS)
    # libstdc++ on the TV ranges from 6.0.16 to 6.0.32 (6.0.25 on webOS 6) and a bundled copy would
    # shadow the TV's own for LG's C++ libraries (docs/research/TOOLCHAIN-AND-BUILD.md, ABI rule 2).
    # -tno-glibc-polyfill: the NDK gcc otherwise links libglibc_polyfills.a (a getauxval shim whose
    # licence is unknown) whenever something references getauxval.
    add_link_options(-static-libstdc++ -static-libgcc -Wl,--build-id=sha1 -Wl,--as-needed -tno-glibc-polyfill)
    # Build paths never reach the binaries, so the same source gives the same packages in any build
    # directory or checkout location (tools/release.sh --check-reproducible).
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=." "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build")
    # Debug info in the build tree for tools/symbolize-crash.py; the ipk's staged copies are stripped.
    add_compile_options(-g)
    # Bundled shared libraries live in lib/ next to the executable.
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
endif ()

add_compile_options(-Wall -Wextra)

# libwebp decoder (static, pinned release) for WebP images in image/.
include(LibWebp)

# libdovi (static, pinned crate, built with cargo) for Dolby Vision Profile 7 -> 8.1 in media/.
include(LibDovi)

# libass and its font stack (static, pinned releases) for styled ASS/SSA subtitles in media/.
include(LibAss)

# SDL2: system SDL on the host, webosbrew SDL-webOS built from a pinned tag on webOS (ADR-003).
if (WNK_WEBOS)
    include(SdlWebOS)
else ()
    find_package(SDL2 2.0.10 REQUIRED CONFIG)
    add_library(wnk_sdl2 INTERFACE)
    target_link_libraries(wnk_sdl2 INTERFACE SDL2::SDL2)
endif ()

add_subdirectory(core)
add_subdirectory(logging)
add_subdirectory(platform)
add_subdirectory(input)
add_subdirectory(focus)
add_subdirectory(render)
add_subdirectory(storage)
add_subdirectory(async)
add_subdirectory(net)
add_subdirectory(image)
add_subdirectory(i18n)
add_subdirectory(audio)
add_subdirectory(ui)
add_subdirectory(diagnostics)
add_subdirectory(app)
include(Ffmpeg)
add_subdirectory(media)
add_subdirectory(services/auth)

if (WNK_BUILD_EXAMPLES)
    include(WebOSPackage)
    add_subdirectory(applications/examples/hello)
    add_subdirectory(applications/examples/tvui)
    add_subdirectory(applications/examples/shows)
    add_subdirectory(applications/examples/player)
endif ()

if (WNK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

# Applications kept outside this tree (cmake/ExtraApps.cmake): a ;-separated list of absolute
# directories, each with its own CMakeLists.txt calling wnk_add_webos_package(). Read from the
# environment on first configure so tools/test.sh and tools/build-webos.sh need no extra arguments.
set(WNK_EXTRA_APP_DIRS "$ENV{WNK_EXTRA_APP_DIRS}" CACHE STRING "Out-of-tree application directories")
if (WNK_EXTRA_APP_DIRS)
    include(WebOSPackage)
    include(ExtraApps)
    wnk_add_extra_app_dirs(${WNK_EXTRA_APP_DIRS})
endif ()
