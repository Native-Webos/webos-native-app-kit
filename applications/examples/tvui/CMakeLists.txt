# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(wnk-tvui main.cpp)
target_link_libraries(wnk-tvui PRIVATE wnk_app wnk_media_dlna)  # DLNA browsing (not the media model)
target_include_directories(wnk-tvui SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")

# Fonts and the poster thumbnails shared with Example A live next to the executable and in the ipk.
set(WNK_FONT_DIR "${PROJECT_SOURCE_DIR}/assets/fonts")
set(WNK_POSTER_DIR "${PROJECT_SOURCE_DIR}/applications/examples/hello/posters")
add_custom_command(TARGET wnk-tvui POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WNK_FONT_DIR}" "$<TARGET_FILE_DIR:wnk-tvui>/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WNK_POSTER_DIR}" "$<TARGET_FILE_DIR:wnk-tvui>/posters"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/media" "$<TARGET_FILE_DIR:wnk-tvui>/media"
        VERBATIM)

wnk_add_webos_package(
        TARGET wnk-tvui
        APPID org.wnk.examples.tvui
        TITLE "WNK TV UI"
        VERSION 0.6.11
        VENDOR "webos-native-app-kit"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
        LARGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/largeIcon.png"
        ICON_COLOR "#2f8f5b"
        EXPECTED_NEEDED "${CMAKE_CURRENT_SOURCE_DIR}/expected-needed.txt"
        EXTRA_DIRS "${WNK_FONT_DIR}" "${WNK_POSTER_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/media")
