# SPDX-License-Identifier: GPL-3.0-or-later
# libcurl is loaded at runtime (dlopen), so nothing here adds a DT_NEEDED entry; the vendored curl
# headers (third_party/curl, curl 7.87.0) provide types and constants only.
add_library(wnk_net STATIC src/http.cpp src/http_stream.cpp src/segment_reader.cpp src/curl_transport.cpp src/curl_api.cpp src/websocket_codec.cpp src/websocket_client.cpp src/lan.cpp src/ssdp.cpp src/http_cache.cpp)
target_include_directories(wnk_net PUBLIC include)
target_include_directories(wnk_net SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/curl/include")
target_link_libraries(wnk_net PUBLIC wnk_async wnk_logging PRIVATE ${CMAKE_DL_LIBS})
