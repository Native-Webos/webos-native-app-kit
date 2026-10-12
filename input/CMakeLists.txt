# SPDX-License-Identifier: GPL-3.0-or-later
# SDL-free core: key events, keymaps, action mapping (unit-tested on the host without SDL).
add_library(wnk_input STATIC src/input.cpp src/gamepad.cpp)
target_include_directories(wnk_input PUBLIC include)
if (WNK_WEBOS)
    target_compile_definitions(wnk_input PRIVATE WNK_PLATFORM_WEBOS=1)
endif ()

# SDL adapter.
add_library(wnk_input_sdl STATIC sdl/sdl_input.cpp)
target_include_directories(wnk_input_sdl PUBLIC sdl)
target_link_libraries(wnk_input_sdl PUBLIC wnk_input wnk_sdl2)
