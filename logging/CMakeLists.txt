# SPDX-License-Identifier: GPL-3.0-or-later
add_library(wnk_logging STATIC src/log.cpp)
target_include_directories(wnk_logging PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(wnk_logging PUBLIC Threads::Threads)
