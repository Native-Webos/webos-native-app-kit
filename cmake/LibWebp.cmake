# SPDX-License-Identifier: GPL-3.0-or-later
#
# libwebp's decoder and demuxer from a pinned release, built with the app's toolchain and linked
# statically into wnk_image (target `wnk_webp`). The TV's own libwebp is not used: it is missing from
# several inventoried firmware images, webOS 5 and 6 among them (tools/fw-lib-matrix.py).
# Licence: BSD-3-Clause plus Google's patent grant: third_party/licenses/libwebp-*.txt, shipped in every package.

include(ExternalProject)

set(WNK_LIBWEBP_VERSION "1.4.0" CACHE STRING "libwebp release")
set(WNK_LIBWEBP_SHA256 "61f873ec69e3be1b99535634340d5bde750b2e4447caa1db9f61be3fd49ab1e5" CACHE STRING "")

set(_wnk_webp_build "${CMAKE_BINARY_DIR}/third_party/libwebp-build")
set(_wnk_webp_src "${_wnk_webp_build}/src/wnk_libwebp_ext")
set(_wnk_webp_bin "${_wnk_webp_build}/src/wnk_libwebp_ext-build")
set(_wnk_webp_args
        "-DCMAKE_BUILD_TYPE:STRING=Release"
        "-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON"
        "-DCMAKE_C_FLAGS:STRING=-ffile-prefix-map=${_wnk_webp_build}=libwebp"
        -DBUILD_SHARED_LIBS=OFF -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF
        -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF
        -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_LIBWEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF)
if (CMAKE_TOOLCHAIN_FILE)
    list(APPEND _wnk_webp_args "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}")
endif ()

set(_wnk_webp_libs "${_wnk_webp_bin}/libwebpdemux.a" "${_wnk_webp_bin}/libwebpdecoder.a")
ExternalProject_Add(wnk_libwebp_ext
        URL "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${WNK_LIBWEBP_VERSION}.tar.gz"
        URL_HASH "SHA256=${WNK_LIBWEBP_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP OFF
        PREFIX "${_wnk_webp_build}"
        CMAKE_ARGS ${_wnk_webp_args}
        BUILD_COMMAND "${CMAKE_COMMAND}" --build . --target webpdecoder webpdemux
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS ${_wnk_webp_libs})

# Include directories must exist at configure time.
file(MAKE_DIRECTORY "${_wnk_webp_src}/src")
find_package(Threads REQUIRED)
add_library(wnk_webp INTERFACE)
target_include_directories(wnk_webp SYSTEM INTERFACE "${_wnk_webp_src}/src")
target_link_libraries(wnk_webp INTERFACE ${_wnk_webp_libs} Threads::Threads m)
add_dependencies(wnk_webp wnk_libwebp_ext)

unset(_wnk_webp_build)
unset(_wnk_webp_bin)
unset(_wnk_webp_args)
unset(_wnk_webp_libs)
