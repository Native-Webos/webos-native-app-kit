# SPDX-License-Identifier: GPL-3.0-or-later
# CPU-side: fonts (stb_truetype), UTF-8, atlas packing. No GL; unit-tested on the host.
add_library(wnk_render STATIC src/font.cpp src/canvas.cpp)
target_include_directories(wnk_render PUBLIC include)
target_include_directories(wnk_render SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/stb")
target_link_libraries(wnk_render PUBLIC wnk_core)

# OpenGL ES 2 backend.
add_library(wnk_render_gles2 STATIC gles2/src/gles2_renderer.cpp)
target_include_directories(wnk_render_gles2 PUBLIC gles2/include)
if (WNK_WEBOS)
    # Firmware GLESv2: same SONAME on every inventoried release (docs/research/COMPATIBILITY.md).
    target_link_libraries(wnk_render_gles2 PUBLIC wnk_render GLESv2)
else ()
    find_library(WNK_GLESV2_LIBRARY GLESv2 REQUIRED)
    target_link_libraries(wnk_render_gles2 PUBLIC wnk_render "${WNK_GLESV2_LIBRARY}")
endif ()
