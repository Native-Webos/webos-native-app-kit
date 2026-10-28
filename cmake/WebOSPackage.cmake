# SPDX-License-Identifier: GPL-3.0-or-later
#
# wnk_add_webos_package(
#     TARGET <executable target>
#     APPID <reverse.dns.id>             lowercase, used for the package and the app directory
#     TITLE <"Launcher title">
#     VERSION <x.y.z>                    three integers (LG's requirement)
#     VENDOR <"Vendor">
#     ICON <80x80 png>  LARGE_ICON <130x130 png>
#     [ICON_COLOR <#rrggbb>]
#     [EXPECTED_NEEDED <file>]           reviewed DT_NEEDED list for the executable
#     [MIN_RELEASE <x.y.z>]              lowest webOS release fw-check grades (default 5.3.1)
#     [EXTRA_FILES <file>...]            copied next to the executable
#     [EXTRA_DIRS <dir>...]              copied next to the executable as <basename>/ (e.g. fonts)
#     [REQUIRED_PERMISSIONS <acg>...]    appinfo.json requiredPermissions (e.g. database.operation)
#     [BUNDLED_LIBS <so>...]             further libraries for lib/ (e.g. WNK_FFMPEG_RUNTIME_LIBS)
#     [PRIVATE_RUNTIMES <dir>...]        self-contained runtimes (own loader and C library) copied as
#                                        <basename>/ and started as separate processes; checked with
#                                        check-package.py --private-runtime instead of the executable's
#                                        ARM/libstdc++ rules
#     [SOURCE_URL <url>]                 where the app's corresponding source is published (named in
#                                        licenses/SOURCE.txt; default WNK_SOURCE_URL, else the kit's notice)
# )
#
# Adds, for webOS builds only:
#   <target>-ipk      stage → tools/mkipk.py → tools/check-package.py   (build/dist/<id>_<ver>_arm.ipk)
#   <target>-verify   tools/verify.sh on that ipk (firmware loadability; webosbrew-ipk-verify if installed)
#   <target>-install  tools/install.sh (ares-install, honours ARES_DEVICE)
#   <target>-launch   tools/launch.sh  (ares-launch)
# and makes wnk-ipks (every app's ipk) depend on <target>-ipk.
#
# Every package carries licenses/: the GPL text, THIRD-PARTY-NOTICES.md, the licences of what is
# compiled in or bundled (SDL, stb, nlohmann/json, curl headers; FFmpeg's LGPL when its libraries are
# bundled) and SOURCE.txt, which says where the corresponding source is (and, with FFmpeg, the exact
# upstream tarball and the build script, as LGPL-2.1 §6 asks).
#
# Staging is plain `cmake -E` so it is readable. The bundled SDL (cmake/SdlWebOS.cmake) is copied to
# lib/libSDL2-2.0.so.0 as a real file. Only the staged copies are stripped; the unstripped executable
# stays in the build tree, matched by build id, for crash symbolisation (PlxNative's practice).

set(WNK_PACKAGE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(WNK_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../tools")

function(wnk_add_webos_package)
    cmake_parse_arguments(P "" "TARGET;APPID;TITLE;VERSION;VENDOR;ICON;LARGE_ICON;ICON_COLOR;EXPECTED_NEEDED;MIN_RELEASE;SOURCE_URL"
            "EXTRA_FILES;EXTRA_DIRS;REQUIRED_PERMISSIONS;BUNDLED_LIBS;PRIVATE_RUNTIMES" ${ARGN})
    foreach (req TARGET APPID TITLE VERSION VENDOR ICON LARGE_ICON)
        if (NOT P_${req})
            message(FATAL_ERROR "wnk_add_webos_package: ${req} is required")
        endif ()
    endforeach ()
    if (NOT P_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
        message(FATAL_ERROR "wnk_add_webos_package: VERSION '${P_VERSION}' must be three integers")
    endif ()
    if (NOT WNK_WEBOS)
        return()
    endif ()
    if (NOT P_ICON_COLOR)
        set(P_ICON_COLOR "#1f2937")
    endif ()
    if (NOT P_MIN_RELEASE)
        set(P_MIN_RELEASE "5.3.1")
    endif ()
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

    set(_exe_name "$<TARGET_FILE_NAME:${P_TARGET}>")
    set(_stage "${CMAKE_BINARY_DIR}/ipk/${P_TARGET}/stage")
    set(_dist "${CMAKE_BINARY_DIR}/dist")
    set(_ipk "${_dist}/${P_APPID}_${P_VERSION}_${WNK_WEBOS_ARCH}.ipk")
    set(_appinfo "${CMAKE_BINARY_DIR}/ipk/${P_TARGET}/appinfo.json")

    # appinfo.json is generated, so id, version and title live only in the app's CMakeLists.
    set(APPINFO_ID "${P_APPID}")
    set(APPINFO_VERSION "${P_VERSION}")
    set(APPINFO_VENDOR "${P_VENDOR}")
    set(APPINFO_TITLE "${P_TITLE}")
    set(APPINFO_ICON_COLOR "${P_ICON_COLOR}")
    set(APPINFO_EXTRA "")
    if (P_REQUIRED_PERMISSIONS)
        list(TRANSFORM P_REQUIRED_PERMISSIONS PREPEND "\"" OUTPUT_VARIABLE _perms)
        list(TRANSFORM _perms APPEND "\"")
        list(JOIN _perms ", " _perms)
        set(APPINFO_EXTRA ",\n    \"requiredPermissions\": [${_perms}]")
    endif ()
    configure_file("${WNK_PACKAGE_MODULE_DIR}/appinfo.json.in" "${_appinfo}.in" @ONLY)
    file(GENERATE OUTPUT "${_appinfo}" INPUT "${_appinfo}.in"
            TARGET ${P_TARGET})

    # licenses/SOURCE.txt
    if (NOT P_SOURCE_URL)
        set(P_SOURCE_URL "${WNK_SOURCE_URL}")
    endif ()
    if (NOT P_SOURCE_URL)
        set(P_SOURCE_URL "(not set: whoever distributes this package must publish the source and name it here;\n     see SOURCE_URL in cmake/WebOSPackage.cmake)")
    endif ()
    set(SOURCE_URL "${P_SOURCE_URL}")
    set(FFMPEG_NOTE "")
    set(_ffmpeg FALSE)
    foreach (_lib IN LISTS P_BUNDLED_LIBS)
        if (_lib MATCHES "-wnk\\.so")
            set(_ffmpeg TRUE)
        endif ()
    endforeach ()
    if (_ffmpeg)
        file(STRINGS "${WNK_TOOLS_DIR}/build-ffmpeg.sh" _ffv REGEX "^VERSION=")
        file(STRINGS "${WNK_TOOLS_DIR}/build-ffmpeg.sh" _ffs REGEX "^SHA256=")
        string(REPLACE "VERSION=" "" _ffv "${_ffv}")
        string(REPLACE "SHA256=" "" _ffs "${_ffs}")
        set(FFMPEG_NOTE "
lib/*-wnk.so.* are FFmpeg ${_ffv} (libavformat, libavcodec, libavutil, libswresample), unmodified,
under the GNU Lesser General Public License 2.1 or later: licenses/FFmpeg-LGPL-2.1.txt. Built without
GPL or non-free parts by tools/build-ffmpeg.sh of the kit (configure options listed there), from
    https://ffmpeg.org/releases/ffmpeg-${_ffv}.tar.xz
    sha256 ${_ffs}
They are shared libraries found through RPATH \$ORIGIN/lib: you may replace them with your own build
of the same version and options.
")
    endif ()
    set(_source_txt "${CMAKE_CURRENT_BINARY_DIR}/${P_TARGET}-SOURCE.txt")
    configure_file("${WNK_PACKAGE_MODULE_DIR}/package-source.txt.in" "${_source_txt}" @ONLY)
    set(_kit "${WNK_PACKAGE_MODULE_DIR}/..")
    set(_licence_files
            "${_kit}/LICENSE" "${_kit}/THIRD-PARTY-NOTICES.md"
            "${_kit}/third_party/licenses/SDL2-zlib.txt" "${_kit}/third_party/stb/LICENSE"
            "${_kit}/third_party/nlohmann/LICENSE.MIT" "${_kit}/third_party/qrcodegen/LICENSE.MIT"
            "${_kit}/third_party/nanosvg/LICENSE.txt" "${_kit}/third_party/licenses/libwebp-BSD-3-Clause.txt"
            "${_kit}/third_party/licenses/libwebp-PATENTS.txt"
            "${_kit}/third_party/licenses/libdovi-crates.txt"
            "${_kit}/third_party/curl/COPYING")

    set(_strip "${WNK_NDK_PREFIX}strip")
    set(_cmds
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_stage}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_stage}/lib" "${_stage}/licenses" "${_dist}"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/LICENSE" "${_stage}/licenses/LICENSE"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/THIRD-PARTY-NOTICES.md" "${_stage}/licenses/THIRD-PARTY-NOTICES.md"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/third_party/licenses/SDL2-zlib.txt" "${_stage}/licenses/SDL2-zlib.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/third_party/stb/LICENSE" "${_stage}/licenses/stb-MIT-or-public-domain.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/third_party/nlohmann/LICENSE.MIT" "${_stage}/licenses/nlohmann-json-MIT.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/third_party/qrcodegen/LICENSE.MIT" "${_stage}/licenses/qrcodegen-MIT.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/third_party/nanosvg/LICENSE.txt" "${_stage}/licenses/nanosvg-zlib.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/third_party/licenses/libwebp-BSD-3-Clause.txt" "${_stage}/licenses/libwebp-BSD-3-Clause.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/third_party/licenses/libwebp-PATENTS.txt" "${_stage}/licenses/libwebp-PATENTS.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/third_party/licenses/libdovi-crates.txt" "${_stage}/licenses/libdovi-crates.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/third_party/curl/COPYING" "${_stage}/licenses/curl-headers.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_source_txt}" "${_stage}/licenses/SOURCE.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy "${_appinfo}" "${_stage}/appinfo.json"
            COMMAND "${CMAKE_COMMAND}" -E copy "${P_ICON}" "${_stage}/icon.png"
            COMMAND "${CMAKE_COMMAND}" -E copy "${P_LARGE_ICON}" "${_stage}/largeIcon.png"
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${P_TARGET}>" "${_stage}/${_exe_name}"
            COMMAND "${_strip}" --strip-unneeded "${_stage}/${_exe_name}")
    if (TARGET wnk_sdl2_shared)
        list(APPEND _cmds
                COMMAND "${CMAKE_COMMAND}" -E copy "${WNK_SDL_WEBOS_LIBRARY}" "${_stage}/lib/libSDL2-2.0.so.0"
                COMMAND "${_strip}" --strip-unneeded "${_stage}/lib/libSDL2-2.0.so.0")
    endif ()
    # Further shared libraries for lib/ (found through RPATH $ORIGIN/lib), copied under their own
    # file name (pass the SONAME file) as real files and stripped.
    foreach (_lib IN LISTS P_BUNDLED_LIBS)
        get_filename_component(_lname "${_lib}" NAME)
        list(APPEND _cmds
                COMMAND "${CMAKE_COMMAND}" -E copy "${_lib}" "${_stage}/lib/${_lname}"
                COMMAND "${_strip}" --strip-unneeded "${_stage}/lib/${_lname}")
    endforeach ()
    if (_ffmpeg)
        list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy "${_kit}/third_party/licenses/LGPL-2.1.txt"
                "${_stage}/licenses/FFmpeg-LGPL-2.1.txt")
        list(APPEND _licence_files "${_kit}/third_party/licenses/LGPL-2.1.txt")
    endif ()
    foreach (_f IN LISTS P_EXTRA_FILES)
        list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy "${_f}" "${_stage}/")
    endforeach ()
    set(_dir_deps)
    foreach (_d IN LISTS P_EXTRA_DIRS)
        get_filename_component(_dname "${_d}" NAME)
        list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_d}" "${_stage}/${_dname}")
        file(GLOB_RECURSE _dfiles CONFIGURE_DEPENDS "${_d}/*")
        list(APPEND _dir_deps ${_dfiles})
    endforeach ()

    set(_check_args)
    if (P_EXPECTED_NEEDED)
        set(_check_args --expected-needed "${P_EXPECTED_NEEDED}")
    endif ()
    set(_verify_args)
    foreach (_d IN LISTS P_PRIVATE_RUNTIMES)
        get_filename_component(_dname "${_d}" NAME)
        list(APPEND _verify_args --private-runtime "${_dname}")
        list(APPEND _cmds COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_d}" "${_stage}/${_dname}")
        file(GLOB_RECURSE _dfiles CONFIGURE_DEPENDS "${_d}/*")
        list(APPEND _dir_deps ${_dfiles})
        list(APPEND _check_args --private-runtime "${_dname}")
    endforeach ()

    add_custom_target(${P_TARGET}-ipk
            ${_cmds}
            COMMAND "${Python3_EXECUTABLE}" "${WNK_TOOLS_DIR}/mkipk.py" --app-dir "${_stage}" --out-dir "${_dist}"
                --arch "${WNK_WEBOS_ARCH}"
            COMMAND "${Python3_EXECUTABLE}" "${WNK_TOOLS_DIR}/check-package.py" "${_ipk}" ${_check_args}
                --arch "${WNK_WEBOS_ARCH}"
            DEPENDS ${P_TARGET} "${_appinfo}" "${P_ICON}" "${P_LARGE_ICON}" ${P_EXTRA_FILES} ${_dir_deps}
                    ${_licence_files} "${_source_txt}"
            BYPRODUCTS "${_ipk}"
            COMMENT "Packaging ${P_APPID} ${P_VERSION}"
            VERBATIM)
    add_custom_target(${P_TARGET}-verify
            COMMAND "${WNK_TOOLS_DIR}/verify.sh" "${_ipk}" --min-release "${P_MIN_RELEASE}"
                --runtime-libs "${WNK_PACKAGE_MODULE_DIR}/sdl-webos-runtime-libs.txt" ${_verify_args}
            DEPENDS ${P_TARGET}-ipk
            COMMENT "Checking ${P_APPID} against firmware inventories"
            VERBATIM USES_TERMINAL)
    add_custom_target(${P_TARGET}-install
            COMMAND "${WNK_TOOLS_DIR}/install.sh" "${_ipk}"
            DEPENDS ${P_TARGET}-ipk
            VERBATIM USES_TERMINAL)
    add_custom_target(${P_TARGET}-launch
            COMMAND "${WNK_TOOLS_DIR}/launch.sh" "${P_APPID}"
            VERBATIM USES_TERMINAL)
    if (NOT TARGET wnk-ipks)
        add_custom_target(wnk-ipks)
    endif ()
    add_dependencies(wnk-ipks ${P_TARGET}-ipk)
endfunction()
