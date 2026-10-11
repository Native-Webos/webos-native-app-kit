# SPDX-License-Identifier: GPL-3.0-or-later
# The bundled FFmpeg (tools/build-ffmpeg.sh): built at configure time into the build tree, once per
# configuration (the script is a no-op when its stamp matches). Provides:
#   wnk_ffmpeg               INTERFACE target: headers + libavformat/libavcodec/libswresample/libavutil (-wnk)
#   WNK_FFMPEG_RUNTIME_LIBS  the SONAME files to ship in an app's lib/ (BUNDLED_LIBS)
option(WNK_FFMPEG "Build the bundled FFmpeg demuxer (needed by media sources and Example D)" ON)
if (NOT WNK_FFMPEG)
    return()
endif ()

set(WNK_FFMPEG_PREFIX "${CMAKE_BINARY_DIR}/ffmpeg" CACHE PATH "Install prefix of the bundled FFmpeg")
if (WNK_WEBOS)
    set(_ff_target arm)
    get_filename_component(_ff_ndk "${WNK_NDK_PREFIX}" DIRECTORY)  # .../bin
    get_filename_component(_ff_ndk "${_ff_ndk}" DIRECTORY)
    set(_ff_env "WEBOS_NDK=${_ff_ndk}")
else ()
    set(_ff_target host)
    set(_ff_env "WNK_FFMPEG_HOST=1")
endif ()
message(STATUS "Bundled FFmpeg (${_ff_target}): ${WNK_FFMPEG_PREFIX}")
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "${_ff_env}" "${PROJECT_SOURCE_DIR}/tools/build-ffmpeg.sh" ${_ff_target} "${WNK_FFMPEG_PREFIX}"
        RESULT_VARIABLE _ff_rc OUTPUT_VARIABLE _ff_out ERROR_VARIABLE _ff_err)
if (NOT _ff_rc EQUAL 0)
    message(FATAL_ERROR "tools/build-ffmpeg.sh failed:\n${_ff_out}\n${_ff_err}\nConfigure with -DWNK_FFMPEG=OFF to build without media sources.")
endif ()

add_library(wnk_ffmpeg INTERFACE)
target_include_directories(wnk_ffmpeg SYSTEM INTERFACE "${WNK_FFMPEG_PREFIX}/include")
set(WNK_FFMPEG_RUNTIME_LIBS)
foreach (_lib avformat avcodec swresample avutil)
    target_link_libraries(wnk_ffmpeg INTERFACE "${WNK_FFMPEG_PREFIX}/lib/lib${_lib}-wnk.so")
    file(GLOB _soname "${WNK_FFMPEG_PREFIX}/lib/lib${_lib}-wnk.so.[0-9]*")
    list(FILTER _soname INCLUDE REGEX "lib${_lib}-wnk\\.so\\.[0-9]+$")
    list(APPEND WNK_FFMPEG_RUNTIME_LIBS ${_soname})
endforeach ()
