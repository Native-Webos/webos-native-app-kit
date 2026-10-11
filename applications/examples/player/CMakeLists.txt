# SPDX-License-Identifier: GPL-3.0-or-later
if (NOT TARGET wnk_media_ffmpeg)
    message(STATUS "Example D (player) skipped: configure with WNK_FFMPEG=ON")
    return()
endif ()

add_executable(wnk-player main.cpp)
target_link_libraries(wnk-player PRIVATE wnk_app wnk_media_ffmpeg wnk_media_ndl)
target_include_directories(wnk-player SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")

# A/V test clips, generated with the host's ffmpeg CLI (tools/make-test-clips.sh).
set(WNK_CLIPS_DIR "${CMAKE_CURRENT_BINARY_DIR}/clips")
set(WNK_CLIPS "${WNK_CLIPS_DIR}/h264-aac.mp4" "${WNK_CLIPS_DIR}/hevc-ac3-5.1.mkv" "${WNK_CLIPS_DIR}/h264-mp3.ts")
find_program(WNK_FFMPEG_CLI ffmpeg)
if (WNK_FFMPEG_CLI)
    add_custom_command(OUTPUT ${WNK_CLIPS}
            COMMAND "${PROJECT_SOURCE_DIR}/tools/make-test-clips.sh" "${WNK_CLIPS_DIR}"
            DEPENDS "${PROJECT_SOURCE_DIR}/tools/make-test-clips.sh"
            COMMENT "Generating player test clips" VERBATIM)
    add_custom_target(wnk-player-clips DEPENDS ${WNK_CLIPS})
    add_dependencies(wnk-player wnk-player-clips)
else ()
    message(WARNING "ffmpeg CLI not found: Example D is built without its test clips")
    file(MAKE_DIRECTORY "${WNK_CLIPS_DIR}")
endif ()

set(WNK_FONT_DIR "${PROJECT_SOURCE_DIR}/assets/fonts")
add_custom_command(TARGET wnk-player POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WNK_FONT_DIR}" "$<TARGET_FILE_DIR:wnk-player>/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WNK_CLIPS_DIR}" "$<TARGET_FILE_DIR:wnk-player>/clips"
        VERBATIM)

wnk_add_webos_package(
        TARGET wnk-player
        APPID org.wnk.examples.player
        TITLE "WNK Player"
        VERSION 0.3.1
        VENDOR "webos-native-app-kit"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
        LARGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/largeIcon.png"
        ICON_COLOR "#b4472f"
        EXPECTED_NEEDED "${CMAKE_CURRENT_SOURCE_DIR}/expected-needed.txt"
        EXTRA_DIRS "${WNK_FONT_DIR}" "${WNK_CLIPS_DIR}"
        BUNDLED_LIBS ${WNK_FFMPEG_RUNTIME_LIBS})
