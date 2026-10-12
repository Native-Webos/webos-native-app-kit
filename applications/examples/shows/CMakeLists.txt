# SPDX-License-Identifier: GPL-3.0-or-later
# The catalogue code has no UI or threads, so the host tests link it on its own (test_shows_catalogue).
add_library(wnk_example_shows_catalogue STATIC catalogue.cpp)
target_include_directories(wnk_example_shows_catalogue PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(wnk_example_shows_catalogue SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(wnk_example_shows_catalogue PUBLIC wnk_net)

add_executable(wnk-shows main.cpp)
target_link_libraries(wnk-shows PRIVATE wnk_app wnk_example_shows_catalogue)
target_include_directories(wnk-shows SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")

set(WNK_FONT_DIR "${PROJECT_SOURCE_DIR}/assets/fonts")
add_custom_command(TARGET wnk-shows POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WNK_FONT_DIR}" "$<TARGET_FILE_DIR:wnk-shows>/fonts"
        VERBATIM)

# database.operation: saved copy and favourites in DB8 (AppConfig::storage).
wnk_add_webos_package(
        TARGET wnk-shows
        APPID org.wnk.examples.shows
        TITLE "WNK Shows"
        VERSION 0.1.0
        VENDOR "webos-native-app-kit"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
        LARGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/largeIcon.png"
        ICON_COLOR "#8a3ffc"
        EXPECTED_NEEDED "${CMAKE_CURRENT_SOURCE_DIR}/expected-needed.txt"
        REQUIRED_PERMISSIONS database.operation
        EXTRA_DIRS "${WNK_FONT_DIR}")
