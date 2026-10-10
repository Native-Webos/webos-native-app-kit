# SPDX-License-Identifier: GPL-3.0-or-later
add_library(wnk_platform STATIC src/process.cpp src/probe.cpp)
target_include_directories(wnk_platform PUBLIC include)
if (WNK_WEBOS)
    target_compile_definitions(wnk_platform PUBLIC WNK_PLATFORM_WEBOS=1)
endif ()
