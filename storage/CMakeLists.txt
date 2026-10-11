# SPDX-License-Identifier: GPL-3.0-or-later
add_library(wnk_storage STATIC src/kv.cpp src/secret.cpp)
target_include_directories(wnk_storage PUBLIC include)
target_include_directories(wnk_storage SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(wnk_storage PUBLIC wnk_platform wnk_logging)
