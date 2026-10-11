# SPDX-License-Identifier: GPL-3.0-or-later
# Media model and playback controller. SDL-free; backends and sources live in their own targets.
add_library(wnk_media STATIC src/media.cpp src/controller.cpp)
target_include_directories(wnk_media PUBLIC include)
target_link_libraries(wnk_media PUBLIC wnk_core wnk_async wnk_logging)
