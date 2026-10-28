# SPDX-License-Identifier: GPL-3.0-or-later
#
# libdovi (quietvoid's dolby_vision crate 3.4.0 with its C API, MIT), built with cargo from the pinned
# wrapper crate in third_party/libdovi (Cargo.lock committed, --locked) and linked statically into
# wnk_media (target `wnk_dovi`). media/src/dovi.cpp uses it to play Dolby Vision Profile 7 as 8.1.
# On webOS the crate is built for armv7-unknown-linux-gnueabi (the NDK's softfp calling convention)
# with the NDK's gcc as linker. Needs rustup's target: `rustup target add armv7-unknown-linux-gnueabi`.
# Licences: third_party/licenses/libdovi-crates.txt (tools/libdovi-licences.py), shipped in every package.
# WNK_DOVI=OFF, or no cargo, builds without it: Profile 7 then plays as its HDR10 base layer.

option(WNK_DOVI "Build libdovi (needs cargo) for Dolby Vision Profile 7 -> 8.1" ON)
set(WNK_HAVE_DOVI 0)
if (NOT WNK_DOVI)
    return()
endif ()
find_program(WNK_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin")
if (NOT WNK_CARGO)
    message(WARNING "cargo not found: building without libdovi (Dolby Vision Profile 7 plays as HDR10)")
    return()
endif ()

set(_wnk_dovi_src "${PROJECT_SOURCE_DIR}/third_party/libdovi")
set(_wnk_dovi_target_dir "${CMAKE_BINARY_DIR}/third_party/libdovi-target")
set(_wnk_dovi_env "CARGO_TARGET_DIR=${_wnk_dovi_target_dir}"
        "RUSTFLAGS=--remap-path-prefix=${_wnk_dovi_src}=libdovi --remap-path-prefix=$ENV{HOME}/.cargo=cargo")
set(_wnk_dovi_args build --release --locked --manifest-path "${_wnk_dovi_src}/Cargo.toml")
if (WNK_WEBOS)
    set(_wnk_dovi_triple armv7-unknown-linux-gnueabi)
    list(APPEND _wnk_dovi_args --target ${_wnk_dovi_triple})
    list(APPEND _wnk_dovi_env "CARGO_TARGET_ARMV7_UNKNOWN_LINUX_GNUEABI_LINKER=${CMAKE_C_COMPILER}")
    set(_wnk_dovi_lib "${_wnk_dovi_target_dir}/${_wnk_dovi_triple}/release/libwnk_dovi.a")
else ()
    set(_wnk_dovi_lib "${_wnk_dovi_target_dir}/release/libwnk_dovi.a")
endif ()

add_custom_command(OUTPUT "${_wnk_dovi_lib}"
        COMMAND "${CMAKE_COMMAND}" -E env --unset=MAKEFLAGS --unset=MFLAGS ${_wnk_dovi_env} "${WNK_CARGO}" ${_wnk_dovi_args}
        DEPENDS "${_wnk_dovi_src}/Cargo.toml" "${_wnk_dovi_src}/Cargo.lock" "${_wnk_dovi_src}/src/lib.rs"
        COMMENT "Building libdovi with cargo"
        VERBATIM)
add_custom_target(wnk_libdovi_build DEPENDS "${_wnk_dovi_lib}")

find_package(Threads REQUIRED)
add_library(wnk_dovi INTERFACE)
target_include_directories(wnk_dovi SYSTEM INTERFACE "${_wnk_dovi_src}/include")
# What rustc's --print native-static-libs names for the crate (std's dependencies).
target_link_libraries(wnk_dovi INTERFACE "${_wnk_dovi_lib}" gcc_s util rt Threads::Threads m ${CMAKE_DL_LIBS})
add_dependencies(wnk_dovi wnk_libdovi_build)
set(WNK_HAVE_DOVI 1)

unset(_wnk_dovi_src)
unset(_wnk_dovi_target_dir)
unset(_wnk_dovi_env)
unset(_wnk_dovi_args)
unset(_wnk_dovi_triple)
unset(_wnk_dovi_lib)
