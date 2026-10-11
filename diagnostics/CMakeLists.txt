# SPDX-License-Identifier: GPL-3.0-or-later
# Frame metrics, process statistics, crash record, capabilities and device info. SDL-free.
add_library(wnk_diagnostics STATIC src/metrics.cpp src/crash.cpp src/capabilities.cpp)
target_include_directories(wnk_diagnostics PUBLIC include)
target_link_libraries(wnk_diagnostics PUBLIC wnk_core ${CMAKE_DL_LIBS})
target_include_directories(wnk_diagnostics SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
