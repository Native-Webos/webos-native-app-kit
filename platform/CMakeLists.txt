# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Threads REQUIRED)
add_library(wnk_platform STATIC src/process.cpp src/probe.cpp src/luna.cpp)
target_link_libraries(wnk_platform PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
target_include_directories(wnk_platform PUBLIC include)
if (WNK_WEBOS)
    target_compile_definitions(wnk_platform PUBLIC WNK_PLATFORM_WEBOS=1)
endif ()
