# SPDX-License-Identifier: GPL-3.0-or-later
add_library(wnk_image STATIC src/decode.cpp src/cache.cpp src/service.cpp)
target_include_directories(wnk_image PUBLIC include)
target_include_directories(wnk_image SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/stb")
target_link_libraries(wnk_image PUBLIC wnk_async wnk_net wnk_logging)

# GPU side: uploads through the renderer interface only.
add_library(wnk_image_textures STATIC src/textures.cpp)
target_link_libraries(wnk_image_textures PUBLIC wnk_image wnk_render)
