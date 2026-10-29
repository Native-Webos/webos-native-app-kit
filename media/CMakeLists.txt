# SPDX-License-Identifier: GPL-3.0-or-later
# Media model and playback controller. SDL-free; backends and sources live in their own targets.
# The XML reader (manifests, TTML, DIDL-Lite) and DLNA browsing stand alone, so a UI app can browse
# media servers without the media model's dependencies (libdovi brings libgcc_s.so.1).
add_library(wnk_media_xml STATIC src/xml.cpp)
target_include_directories(wnk_media_xml PUBLIC include)
add_library(wnk_media_dlna STATIC src/dlna.cpp)
target_link_libraries(wnk_media_dlna PUBLIC wnk_media_xml wnk_core wnk_async wnk_net PRIVATE wnk_logging)

add_library(wnk_media STATIC src/media.cpp src/controller.cpp src/subtitles.cpp src/device_caps.cpp src/auto_backend.cpp
        src/manifest.cpp src/webvtt.cpp src/ttml.cpp src/bitstream.cpp src/playback_headers.cpp src/iptv.cpp src/dovi.cpp src/time_stretch.cpp)
target_include_directories(wnk_media PUBLIC include)
target_compile_definitions(wnk_media PRIVATE WNK_HAVE_DOVI=${WNK_HAVE_DOVI})
if (WNK_HAVE_DOVI)
    target_link_libraries(wnk_media PRIVATE wnk_dovi)
endif ()
target_include_directories(wnk_media SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(wnk_media PUBLIC wnk_media_xml wnk_media_dlna wnk_core wnk_async wnk_logging wnk_net PRIVATE wnk_platform)  # wnk_net for net::Headers

# MediaSource on the bundled FFmpeg; network URIs go through the kit's HTTP client.
if (TARGET wnk_ffmpeg)
    add_library(wnk_media_ffmpeg STATIC src/ffmpeg_source.cpp src/adaptive_source.cpp src/clearkey.cpp)
    target_link_libraries(wnk_media_ffmpeg PUBLIC wnk_media wnk_net PRIVATE wnk_ffmpeg)
    target_include_directories(wnk_media_ffmpeg SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
endif ()

# LG NDL DirectMedia v2 backend. Builds everywhere (the library is dlopened); only works on webOS 5.3.1+.
add_library(wnk_media_ndl STATIC src/ndl_backend.cpp)
target_link_libraries(wnk_media_ndl PUBLIC wnk_media PRIVATE wnk_diagnostics wnk_platform ${CMAKE_DL_LIBS})
if (WNK_WEBOS)
    target_sources(wnk_media_ndl PRIVATE src/ndl_abi_check.c)  # layout asserts against the NDK header
endif ()

# Drawing subtitles over the video with the UI toolkit (keeps ui/ free of media types).
add_library(wnk_media_ui STATIC src/subtitle_view.cpp src/ass_renderer.cpp)
target_link_libraries(wnk_media_ui PUBLIC wnk_media wnk_ui)
if (WNK_HAVE_LIBASS)
    target_link_libraries(wnk_media_ui PRIVATE wnk_libass)
endif ()

# LG StarfishMediaAPIs backend (webOS 3.4+). Builds everywhere: libplayerAPIs and libAcbAPI are
# dlopened and the SDL-webOS exported-window functions are looked up at runtime.
add_library(wnk_media_starfish STATIC src/starfish_backend.cpp)
target_link_libraries(wnk_media_starfish PUBLIC wnk_media PRIVATE wnk_logging wnk_platform ${CMAKE_DL_LIBS})
target_include_directories(wnk_media_starfish SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
