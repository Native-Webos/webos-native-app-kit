# SPDX-License-Identifier: GPL-3.0-or-later
#
# Decide whether this configuration targets a webOS TV or the development host.
#
# Sets WNK_WEBOS / WNK_HOST and WNK_WEBOS_ARCH ("arm") in the caller's scope. A webOS build is a
# build whose C compiler is the openlgtv buildroot NDK's arm-webos-linux-gnueabi-gcc, selected with
#   -DCMAKE_TOOLCHAIN_FILE=<ndk>/share/buildroot/toolchainfile.cmake
# (the same detection moonlight-tv and webosbrew/samples use).

get_filename_component(_wnk_cc_name "${CMAKE_C_COMPILER}" NAME)

if (_wnk_cc_name MATCHES "^arm-webos-linux-gnueabi-")
    set(WNK_WEBOS ON)
    set(WNK_HOST OFF)
    set(WNK_WEBOS_ARCH "arm")
    get_filename_component(WNK_NDK_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
    set(WNK_NDK_PREFIX "${WNK_NDK_BIN}/arm-webos-linux-gnueabi-")
    message(STATUS "wnk: target webOS (${WNK_WEBOS_ARCH}), NDK ${WNK_NDK_BIN}")
else ()
    set(WNK_WEBOS OFF)
    set(WNK_HOST ON)
    set(WNK_WEBOS_ARCH "")
    message(STATUS "wnk: target host (${CMAKE_SYSTEM_NAME})")
endif ()

unset(_wnk_cc_name)
