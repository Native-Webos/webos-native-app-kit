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
# )
#
# Adds, for webOS builds only:
#   <target>-ipk      stage → tools/mkipk.py → tools/check-package.py   (build/dist/<id>_<ver>_arm.ipk)
#   <target>-verify   tools/verify.sh on that ipk (firmware loadability; webosbrew-ipk-verify if installed)
#   <target>-install  tools/install.sh (ares-install, honours ARES_DEVICE)
#   <target>-launch   tools/launch.sh  (ares-launch)
# and makes wnk-ipks (every app's ipk) depend on <target>-ipk.
#
# Staging is plain `cmake -E` so it is readable. The bundled SDL (cmake/SdlWebOS.cmake) is copied to
# lib/libSDL2-2.0.so.0 as a real file. Only the staged copies are stripped; the unstripped executable
# stays in the build tree, matched by build id, for crash symbolisation (PlxNative's practice).

set(WNK_PACKAGE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(WNK_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../tools")

function(wnk_add_webos_package)
    cmake_parse_arguments(P "" "TARGET;APPID;TITLE;VERSION;VENDOR;ICON;LARGE_ICON;ICON_COLOR;EXPECTED_NEEDED;MIN_RELEASE"
            "EXTRA_FILES;EXTRA_DIRS;REQUIRED_PERMISSIONS" ${ARGN})
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

    set(_strip "${WNK_NDK_PREFIX}strip")
    set(_cmds
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_stage}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_stage}/lib" "${_dist}"
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

    add_custom_target(${P_TARGET}-ipk
            ${_cmds}
            COMMAND "${Python3_EXECUTABLE}" "${WNK_TOOLS_DIR}/mkipk.py" --app-dir "${_stage}" --out-dir "${_dist}"
                --arch "${WNK_WEBOS_ARCH}"
            COMMAND "${Python3_EXECUTABLE}" "${WNK_TOOLS_DIR}/check-package.py" "${_ipk}" ${_check_args}
                --arch "${WNK_WEBOS_ARCH}"
            DEPENDS ${P_TARGET} "${_appinfo}" "${P_ICON}" "${P_LARGE_ICON}" ${P_EXTRA_FILES} ${_dir_deps}
            BYPRODUCTS "${_ipk}"
            COMMENT "Packaging ${P_APPID} ${P_VERSION}"
            VERBATIM)
    add_custom_target(${P_TARGET}-verify
            COMMAND "${WNK_TOOLS_DIR}/verify.sh" "${_ipk}" --min-release "${P_MIN_RELEASE}"
                --runtime-libs "${WNK_PACKAGE_MODULE_DIR}/sdl-webos-runtime-libs.txt"
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
