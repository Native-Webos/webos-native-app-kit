# SPDX-License-Identifier: GPL-3.0-or-later
# Lifecycle state machine: SDL-free, unit-tested on the host.
add_library(wnk_lifecycle STATIC src/lifecycle.cpp)
target_include_directories(wnk_lifecycle PUBLIC include)

find_package(Threads REQUIRED)

# The app shell: SDL, window, GLES2 context, main loop, shutdown sequence.
add_library(wnk_app STATIC src/app.cpp src/overlay.cpp)
target_link_libraries(wnk_app PUBLIC wnk_lifecycle wnk_diagnostics wnk_ui wnk_ui_images wnk_platform wnk_logging
        wnk_storage wnk_async wnk_net wnk_input_sdl wnk_render_gles2 wnk_sdl2 Threads::Threads)
target_include_directories(wnk_app SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
