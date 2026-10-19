# SPDX-License-Identifier: GPL-3.0-or-later
#
# Applications kept outside this tree (a product repository with the kit as a submodule or checkout).
#
# wnk_add_extra_app_dirs(<dir>...)
#   Adds each directory with add_subdirectory(), after the kit's own targets, with its binary
#   directory at <build>/extra-apps/<basename>. Each one has its own CMakeLists.txt and may call
#   wnk_add_webos_package() and add_test() exactly as an app under applications/ does. Directories
#   must be absolute, hold a CMakeLists.txt and have distinct basenames.

function(wnk_add_extra_app_dirs)
    set(_seen)
    foreach (_dir IN LISTS ARGN)
        string(REGEX REPLACE "/+$" "" _dir "${_dir}")
        if (NOT IS_ABSOLUTE "${_dir}")
            message(FATAL_ERROR "WNK_EXTRA_APP_DIRS: '${_dir}' is not an absolute path")
        endif ()
        if (NOT EXISTS "${_dir}/CMakeLists.txt")
            message(FATAL_ERROR "WNK_EXTRA_APP_DIRS: '${_dir}' has no CMakeLists.txt")
        endif ()
        get_filename_component(_name "${_dir}" NAME)
        if (_name IN_LIST _seen)
            message(FATAL_ERROR "WNK_EXTRA_APP_DIRS: two directories are named '${_name}'")
        endif ()
        list(APPEND _seen "${_name}")
        add_subdirectory("${_dir}" "${CMAKE_BINARY_DIR}/extra-apps/${_name}")
    endforeach ()
endfunction()
