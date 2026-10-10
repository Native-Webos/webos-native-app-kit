# SPDX-License-Identifier: GPL-3.0-or-later
add_library(wnk_focus STATIC src/focus.cpp)
target_include_directories(wnk_focus PUBLIC include)
target_link_libraries(wnk_focus PUBLIC wnk_core)
