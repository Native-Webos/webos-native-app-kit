# SPDX-License-Identifier: GPL-3.0-or-later
# Mixer and decoders: SDL-free, unit-tested on the host. stb_vorbis is compiled as C, warnings off
# (third-party code, unmodified).
add_library(wnk_audio STATIC src/audio.cpp "${PROJECT_SOURCE_DIR}/third_party/stb/stb_vorbis.c")
target_include_directories(wnk_audio PUBLIC include)
target_include_directories(wnk_audio SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/stb")
set_source_files_properties("${PROJECT_SOURCE_DIR}/third_party/stb/stb_vorbis.c" PROPERTIES COMPILE_OPTIONS "-w")

# The device: SDL audio.
add_library(wnk_audio_sdl STATIC sdl/src/sdl_output.cpp)
target_include_directories(wnk_audio_sdl PUBLIC sdl/include)
target_link_libraries(wnk_audio_sdl PUBLIC wnk_audio wnk_sdl2)
