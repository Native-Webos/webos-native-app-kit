# SPDX-License-Identifier: GPL-3.0-or-later
# Media model and playback controller. SDL-free; backends and sources live in their own targets.
add_library(wnk_media STATIC src/media.cpp src/controller.cpp src/subtitles.cpp)
target_include_directories(wnk_media PUBLIC include)
target_link_libraries(wnk_media PUBLIC wnk_core wnk_async wnk_logging)

# MediaSource on the bundled FFmpeg; network URIs go through the kit's HTTP client.
if (TARGET wnk_ffmpeg)
    add_library(wnk_media_ffmpeg STATIC src/ffmpeg_source.cpp)
    target_link_libraries(wnk_media_ffmpeg PUBLIC wnk_media wnk_net PRIVATE wnk_ffmpeg)
endif ()

# LG NDL DirectMedia v2 backend. Builds everywhere (the library is dlopened); only works on webOS 5.3.1+.
add_library(wnk_media_ndl STATIC src/ndl_backend.cpp)
target_link_libraries(wnk_media_ndl PUBLIC wnk_media PRIVATE wnk_diagnostics ${CMAKE_DL_LIBS})
if (WNK_WEBOS)
    target_sources(wnk_media_ndl PRIVATE src/ndl_abi_check.c)  # layout asserts against the NDK header
endif ()

# Drawing subtitles over the video with the UI toolkit (keeps ui/ free of media types).
add_library(wnk_media_ui STATIC src/subtitle_view.cpp)
target_link_libraries(wnk_media_ui PUBLIC wnk_media wnk_ui)

# LG StarfishMediaAPIs backend (webOS 5.3.1+). Builds everywhere: libplayerAPIs is dlopened and the
# SDL-webOS exported-window functions are looked up at runtime.
add_library(wnk_media_starfish STATIC src/starfish_backend.cpp)
target_link_libraries(wnk_media_starfish PUBLIC wnk_media PRIVATE wnk_logging ${CMAKE_DL_LIBS})
