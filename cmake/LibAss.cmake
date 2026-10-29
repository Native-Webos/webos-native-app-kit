# SPDX-License-Identifier: GPL-3.0-or-later
#
# libass and its font stack from pinned releases (the same versions and options as PlxNative's
# ci/build-libass.py), built with the app's toolchain and linked statically into wnk_media_ui (target
# `wnk_libass`), so styled ASS/SSA subtitles render the same on every firmware. Nothing comes from the
# TV: its FreeType and HarfBuzz differ between firmware images and are not part of the NDK contract.
#
#   FreeType 2.15.0    font loading and rasterising (no zlib, bzip2, PNG, Brotli, HarfBuzz, HVF)
#   FriBidi 1.0.17     bidirectional text
#   HarfBuzz 15.0.0    shaping
#   libunibreak 8.0    Unicode line breaking (ASS_FEATURE_WRAP_UNICODE)
#   libass 0.17.5      no font provider (no fontconfig): fonts are the media's attachments plus the
#                      default font the app names
#
# Licences (third_party/licenses/libass-*.txt, shipped in every package): FreeType FTL, FriBidi
# LGPL-2.1-or-later, HarfBuzz MIT, libunibreak Zlib, libass ISC.
# WNK_LIBASS=OFF builds without it: ASS subtitles then show as plain text.

option(WNK_LIBASS "Build libass for styled ASS/SSA subtitles" ON)
set(WNK_HAVE_LIBASS 0)
if (NOT WNK_LIBASS)
    return()
endif ()
# The build machine's own pkg-config: it only ever searches this stack's prefix. (The NDK's wrapper
# would put its sysroot in front of every path.)
find_program(WNK_HOST_PKG_CONFIG NAMES pkgconf pkg-config PATHS /usr/bin /usr/local/bin NO_DEFAULT_PATH)
if (NOT WNK_HOST_PKG_CONFIG)
    find_program(WNK_HOST_PKG_CONFIG NAMES pkgconf pkg-config NO_CMAKE_FIND_ROOT_PATH)
endif ()
if (NOT WNK_HOST_PKG_CONFIG)
    message(WARNING "pkg-config not found: building without libass (ASS subtitles show as plain text)")
    return()
endif ()

include(ExternalProject)

set(_ass_root "${CMAKE_BINARY_DIR}/third_party/libass")
set(_ass_prefix "${_ass_root}/prefix")
set(_ass_flags "-O2 -fPIC -fvisibility=hidden -ffile-prefix-map=${_ass_root}=libass")
set(_ass_cmake_args
        "-DCMAKE_BUILD_TYPE:STRING=Release" "-DBUILD_SHARED_LIBS:BOOL=OFF"
        "-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON" "-DCMAKE_INSTALL_LIBDIR:STRING=lib"
        "-DCMAKE_INSTALL_PREFIX:PATH=${_ass_prefix}" "-DCMAKE_PREFIX_PATH:PATH=${_ass_prefix}"
        "-DCMAKE_C_FLAGS:STRING=${_ass_flags}" "-DCMAKE_CXX_FLAGS:STRING=${_ass_flags}")
if (CMAKE_TOOLCHAIN_FILE)
    list(APPEND _ass_cmake_args "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}")
endif ()
# Autotools: the same compilers, and pkg-config sees only what was built here.
set(_ass_env "${CMAKE_COMMAND}" -E env --unset=MAKEFLAGS --unset=MFLAGS
        "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}" "AR=${CMAKE_AR}" "RANLIB=${CMAKE_RANLIB}"
        "CFLAGS=${_ass_flags}" "CXXFLAGS=${_ass_flags}"
        "PKG_CONFIG=${WNK_HOST_PKG_CONFIG}" "PKG_CONFIG_LIBDIR=${_ass_prefix}/lib/pkgconfig" "PKG_CONFIG_PATH=")
set(_ass_configure --prefix=${_ass_prefix} --libdir=${_ass_prefix}/lib --disable-shared --enable-static --with-pic)
if (WNK_WEBOS)
    list(APPEND _ass_configure --host=arm-webos-linux-gnueabi)
endif ()

ExternalProject_Add(wnk_freetype_ext
        URL "https://download.savannah.gnu.org/releases/freetype/freetype-2.15.0.tar.xz"
            "https://downloads.sourceforge.net/project/freetype/freetype2/2.15.0/freetype-2.15.0.tar.xz"
        URL_HASH "SHA256=93cd47eb719c325107f2d3cf087d06299e944c3b48a40b55537302304dea1dd5"
        DOWNLOAD_EXTRACT_TIMESTAMP OFF
        PREFIX "${_ass_root}/freetype"
        CMAKE_ARGS ${_ass_cmake_args} -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON
            -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_HVF=ON
        BUILD_BYPRODUCTS "${_ass_prefix}/lib/libfreetype.a")

ExternalProject_Add(wnk_fribidi_ext
        URL "https://github.com/fribidi/fribidi/releases/download/v1.0.17/fribidi-1.0.17.tar.xz"
        URL_HASH "SHA256=6949dcde27d41cebad1fd741fcafc36d55a1020d2d872d4a6eb3914caabbada2"
        DOWNLOAD_EXTRACT_TIMESTAMP ON  # keep configure newer than configure.ac (no autoreconf)
        PREFIX "${_ass_root}/fribidi"
        CONFIGURE_COMMAND ${_ass_env} <SOURCE_DIR>/configure ${_ass_configure}
        # The library alone: the CLI and manual pages need help2man and cannot run ARM programs.
        BUILD_COMMAND ${_ass_env} make -C lib
        INSTALL_COMMAND ${_ass_env} make -C lib install COMMAND ${_ass_env} make install-pkgconfigDATA
        BUILD_BYPRODUCTS "${_ass_prefix}/lib/libfribidi.a")

ExternalProject_Add(wnk_harfbuzz_ext
        URL "https://github.com/harfbuzz/harfbuzz/releases/download/15.0.0/harfbuzz-15.0.0.tar.xz"
        URL_HASH "SHA256=968536e9abd782f07f7954044c0c4020199fcdd39b41d221ec42d2a3be4a44df"
        DOWNLOAD_EXTRACT_TIMESTAMP OFF
        PREFIX "${_ass_root}/harfbuzz"
        DEPENDS wnk_freetype_ext
        CMAKE_ARGS ${_ass_cmake_args} -DHB_HAVE_FREETYPE=ON
            "-DFREETYPE_LIBRARY:FILEPATH=${_ass_prefix}/lib/libfreetype.a"
            "-DFREETYPE_INCLUDE_DIRS:PATH=${_ass_prefix}/include/freetype2"
            -DHB_HAVE_CORETEXT=OFF -DHB_BUILD_UTILS=OFF -DHB_BUILD_SUBSET=OFF -DHB_BUILD_RASTER=OFF
            -DHB_BUILD_VECTOR=OFF -DHB_BUILD_GPU=OFF
        BUILD_BYPRODUCTS "${_ass_prefix}/lib/libharfbuzz.a")

ExternalProject_Add(wnk_unibreak_ext
        URL "https://github.com/adah1972/libunibreak/releases/download/libunibreak_8_0/libunibreak-8.0.tar.gz"
        URL_HASH "SHA256=9c4fad6e517338a098373acc9f35579ae2c325e6446666fb9ac2666ba15ceba4"
        DOWNLOAD_EXTRACT_TIMESTAMP ON  # keep configure newer than configure.ac (no autoreconf)
        PREFIX "${_ass_root}/unibreak"
        CONFIGURE_COMMAND ${_ass_env} <SOURCE_DIR>/configure ${_ass_configure}
        BUILD_COMMAND ${_ass_env} make
        INSTALL_COMMAND ${_ass_env} make install
        BUILD_BYPRODUCTS "${_ass_prefix}/lib/libunibreak.a")

ExternalProject_Add(wnk_libass_ext
        URL "https://github.com/libass/libass/releases/download/0.17.5/libass-0.17.5.tar.xz"
        URL_HASH "SHA256=2dca25c0e0c837ddf00b52011b3f82cac1e4ddd3ad018227806b0c2288864acc"
        DOWNLOAD_EXTRACT_TIMESTAMP ON  # keep configure newer than configure.ac (no autoreconf)
        PREFIX "${_ass_root}/libass"
        DEPENDS wnk_freetype_ext wnk_fribidi_ext wnk_harfbuzz_ext wnk_unibreak_ext
        CONFIGURE_COMMAND ${_ass_env} <SOURCE_DIR>/configure ${_ass_configure}
            --disable-fontconfig --disable-coretext --disable-directwrite --disable-require-system-font-provider
            --enable-libunibreak --disable-test --disable-compare --disable-profile --disable-asm
        BUILD_COMMAND ${_ass_env} make
        INSTALL_COMMAND ${_ass_env} make install
        BUILD_BYPRODUCTS "${_ass_prefix}/lib/libass.a")

# Include directories must exist at configure time.
file(MAKE_DIRECTORY "${_ass_prefix}/include")
add_library(wnk_libass INTERFACE)
target_include_directories(wnk_libass SYSTEM INTERFACE "${_ass_prefix}/include")
# Order matters for static linking: users before what they use.
target_link_libraries(wnk_libass INTERFACE "${_ass_prefix}/lib/libass.a" "${_ass_prefix}/lib/libunibreak.a"
        "${_ass_prefix}/lib/libharfbuzz.a" "${_ass_prefix}/lib/libfribidi.a" "${_ass_prefix}/lib/libfreetype.a" m)
target_compile_definitions(wnk_libass INTERFACE WNK_HAVE_LIBASS=1)
add_dependencies(wnk_libass wnk_libass_ext)
set(WNK_HAVE_LIBASS 1)

unset(_ass_root)
unset(_ass_flags)
unset(_ass_cmake_args)
unset(_ass_env)
unset(_ass_configure)
