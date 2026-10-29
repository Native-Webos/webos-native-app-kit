# SPDX-License-Identifier: GPL-3.0-or-later
#
# Language level, warnings and the webOS link rules, for the directory that includes this and those
# below it: the kit's own top level, and an app's project through cmake/WnkApp.cmake (an app's
# executable needs the same static libstdc++ and $ORIGIN/lib RPATH as the kit's examples).
# Expects WNK_WEBOS from cmake/WebOSTarget.cmake. Directories below inherit the options, so the kit
# under an app's project does not add them again.
if (WNK_FLAGS_APPLIED)
    return()
endif ()
set(WNK_FLAGS_APPLIED ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (WNK_WEBOS)
    # libstdc++ on the TV ranges from 6.0.16 to 6.0.32 (6.0.25 on webOS 6) and a bundled copy would
    # shadow the TV's own for LG's C++ libraries (docs/research/TOOLCHAIN-AND-BUILD.md, ABI rule 2).
    # -tno-glibc-polyfill: the NDK gcc otherwise links libglibc_polyfills.a (a getauxval shim whose
    # licence is unknown) whenever something references getauxval.
    add_link_options(-static-libstdc++ -static-libgcc -Wl,--build-id=sha1 -Wl,--as-needed -tno-glibc-polyfill)
    # Build paths never reach the binaries, so the same source gives the same packages in any build
    # directory or checkout location (tools/release.sh --check-reproducible).
    get_filename_component(_wnk_flags_kit "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=." "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build")
    if (NOT _wnk_flags_kit STREQUAL CMAKE_SOURCE_DIR)
        add_compile_options("-ffile-prefix-map=${_wnk_flags_kit}=wnk")  # the kit, outside an app's tree
    endif ()
    unset(_wnk_flags_kit)
    # Debug info in the build tree for tools/symbolize-crash.py; the ipk's staged copies are stripped.
    add_compile_options(-g)
    # Bundled shared libraries live in lib/ next to the executable.
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
endif ()

add_compile_options(-Wall -Wextra)
