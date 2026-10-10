# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(wnk-hello main.cpp)
target_link_libraries(wnk-hello PRIVATE wnk_platform wnk_logging wnk_sdl2 ${CMAKE_DL_LIBS})
if (WNK_WEBOS)
    # GLESv2 from the NDK sysroot: present under this SONAME on every inventoried firmware.
    target_link_libraries(wnk-hello PRIVATE GLESv2)
else ()
    find_library(WNK_GLESV2_LIBRARY GLESv2 REQUIRED)
    target_link_libraries(wnk-hello PRIVATE "${WNK_GLESV2_LIBRARY}")
endif ()

wnk_add_webos_package(
        TARGET wnk-hello
        APPID org.wnk.examples.hello
        TITLE "WNK Hello"
        VERSION 0.1.0
        VENDOR "webos-native-app-kit"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
        LARGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/largeIcon.png"
        ICON_COLOR "#1f6feb"
        EXPECTED_NEEDED "${CMAKE_CURRENT_SOURCE_DIR}/expected-needed.txt")
