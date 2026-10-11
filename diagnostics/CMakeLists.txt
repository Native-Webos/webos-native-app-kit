# SPDX-License-Identifier: GPL-3.0-or-later
# Frame metrics, process statistics. SDL-free.
add_library(wnk_diagnostics STATIC src/metrics.cpp)
target_include_directories(wnk_diagnostics PUBLIC include)
target_link_libraries(wnk_diagnostics PUBLIC wnk_core)
