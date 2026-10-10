# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(wnk-hello main.cpp)
target_link_libraries(wnk-hello PRIVATE wnk_platform wnk_logging wnk_storage wnk_input_sdl wnk_focus wnk_render_gles2 wnk_sdl2
        ${CMAKE_DL_LIBS})

# Fonts (Atkinson Hyperlegible, OFL) live in fonts/ next to the executable, on the host and in the ipk.
set(WNK_FONT_DIR "${PROJECT_SOURCE_DIR}/assets/fonts")
add_custom_command(TARGET wnk-hello POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WNK_FONT_DIR}" "$<TARGET_FILE_DIR:wnk-hello>/fonts"
        VERBATIM)

wnk_add_webos_package(
        TARGET wnk-hello
        APPID org.wnk.examples.hello
        TITLE "WNK Hello"
        VERSION 0.4.0
        VENDOR "webos-native-app-kit"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
        LARGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/largeIcon.png"
        ICON_COLOR "#1f6feb"
        EXPECTED_NEEDED "${CMAKE_CURRENT_SOURCE_DIR}/expected-needed.txt"
        EXTRA_DIRS "${WNK_FONT_DIR}"
        REQUIRED_PERMISSIONS database.operation)
