# SPDX-License-Identifier: GPL-3.0-or-later
# Optional application service: sign-in state, stored credentials, authorized requests. Apps that have
# no accounts do not link it; nothing in the platform layers depends on it.
add_library(wnk_auth STATIC src/credentials.cpp src/session.cpp)
target_include_directories(wnk_auth PUBLIC include)
target_include_directories(wnk_auth SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(wnk_auth PUBLIC wnk_storage wnk_net wnk_async wnk_logging)
