# SPDX-License-Identifier: GPL-3.0-or-later
# Widgets, layout, navigation. Draws through the Renderer interface only (no GL, no SDL).
add_library(wnk_ui STATIC src/view.cpp src/ui.cpp src/widgets.cpp src/collection.cpp src/navigation.cpp
        src/text_field.cpp src/forms.cpp "${PROJECT_SOURCE_DIR}/third_party/qrcodegen/qrcodegen.cpp")
target_include_directories(wnk_ui PUBLIC include)
target_link_libraries(wnk_ui PUBLIC wnk_core wnk_input wnk_focus wnk_render)
target_include_directories(wnk_ui SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/qrcodegen")

# ImageSource on top of the image service and texture cache.
add_library(wnk_ui_images STATIC src/image_source.cpp)
target_link_libraries(wnk_ui_images PUBLIC wnk_ui wnk_image_textures)
