# SPDX-License-Identifier: GPL-3.0-or-later
#
# Use the kit from an app's own CMake project, in its own folder and build directory:
#
#   cmake_minimum_required(VERSION 3.22)
#   project(myapp LANGUAGES C CXX)
#   set(WNK_KIT_DIR "../webos-native-app-kit" CACHE PATH "The webOS native app kit")
#   include("${WNK_KIT_DIR}/cmake/WnkApp.cmake")
#   add_executable(myapp main.cpp)
#   target_link_libraries(myapp PRIVATE wnk_app)
#   wnk_add_webos_package(TARGET myapp ...)
#
# tools/new-app.py writes such a project. Including this file:
#   - decides host or webOS (cmake/WebOSTarget.cmake: WNK_WEBOS, WNK_HOST) and applies the kit's
#     compiler and link rules to the app (cmake/WnkFlags.cmake);
#   - adds the kit's libraries (wnk_app, wnk_media_*, ...) without its examples or tests, built only
#     as far as the app links them; its dependencies build into this build directory;
#   - provides wnk_add_webos_package() (cmake/WebOSPackage.cmake: <build>/dist/<id>_<ver>_arm.ipk,
#     package-checked) and wnk_add_app_test() below;
#   - sets WNK_KIT_DIR (absolute), WNK_FONT_DIR (the bundled fonts) and enables testing on the host.
# FFmpeg (media playback) is off unless the app sets WNK_FFMPEG ON before including this file.

get_filename_component(WNK_KIT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if (NOT EXISTS "${WNK_KIT_DIR}/cmake/WebOSPackage.cmake")
    message(FATAL_ERROR "WnkApp.cmake: ${WNK_KIT_DIR} is not the webOS native app kit")
endif ()
if (WNK_KIT_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "WnkApp.cmake is for an app's own project; the kit's examples use the kit's CMakeLists")
endif ()

list(APPEND CMAKE_MODULE_PATH "${WNK_KIT_DIR}/cmake")
include(WebOSTarget)
include(WnkFlags)

option(WNK_FFMPEG "Build the bundled FFmpeg (media playback)" OFF)
set(WNK_BUILD_EXAMPLES OFF CACHE BOOL "The kit's examples (off in an app's project)")
set(WNK_BUILD_TESTS OFF CACHE BOOL "The kit's own tests (off in an app's project)")
set(WNK_FONT_DIR "${WNK_KIT_DIR}/assets/fonts")

add_subdirectory("${WNK_KIT_DIR}" "${CMAKE_BINARY_DIR}/wnk-kit" EXCLUDE_FROM_ALL)
include(WebOSPackage)

if (WNK_HOST)
    enable_testing()
endif ()

# wnk_add_app_test(NAME <name> TARGET <executable> PREFIX <script_env_prefix> KEYS <k1,k2,...>
#                  PASS <regex> [FAIL <regex>] [ARGS <arg>...])
# A host run of the app on SDL's offscreen driver with scripted keys (<PREFIX>_KEYS); passes when the
# log matches PASS, fails on FAIL or a sanitizer report. Host builds only.
function(wnk_add_app_test)
    cmake_parse_arguments(T "" "NAME;TARGET;PREFIX;KEYS;PASS;FAIL" "ARGS" ${ARGN})
    if (NOT WNK_HOST)
        return()
    endif ()
    foreach (req NAME TARGET PREFIX KEYS PASS)
        if (NOT T_${req})
            message(FATAL_ERROR "wnk_add_app_test: ${req} is required")
        endif ()
    endforeach ()
    set(_fail "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|ThreadSanitizer")
    if (T_FAIL)
        set(_fail "${T_FAIL}|${_fail}")
    endif ()
    add_test(NAME ${T_NAME} COMMAND ${T_TARGET} ${T_ARGS})
    set_tests_properties(${T_NAME} PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;LSAN_OPTIONS=suppressions=${WNK_KIT_DIR}/tests/lsan-host.supp;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-data/${T_NAME};${T_PREFIX}_KEYS=${T_KEYS}"
            PASS_REGULAR_EXPRESSION "${T_PASS}"
            FAIL_REGULAR_EXPRESSION "${_fail}"
            TIMEOUT 30)
endfunction()
