# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Threads REQUIRED)
add_library(wnk_async STATIC src/async.cpp)
target_include_directories(wnk_async PUBLIC include)
target_link_libraries(wnk_async PUBLIC Threads::Threads)
