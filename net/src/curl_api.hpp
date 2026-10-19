// SPDX-License-Identifier: GPL-3.0-or-later
//
// The system libcurl.so.4, loaded once at runtime (internal to wnk_net). Compiled against the vendored
// curl 7.87.0 headers; only long-standing API is used so the older curl in webOS 5/6 firmware works.
#pragma once

// CURLOPT_PROTOCOLS is deprecated since curl 7.85 in favour of CURLOPT_PROTOCOLS_STR, which the older
// curl in webOS 5/6 firmware does not have. The numeric form keeps working in new curl too.
#define CURL_DISABLE_DEPRECATION 1
#include <curl/curl.h>

#include <string>

namespace wnk::net {

struct CurlApi {
    CURLcode (*global_init)(long) = nullptr;
    CURL *(*easy_init)() = nullptr;
    CURLcode (*easy_setopt)(CURL *, CURLoption, ...) = nullptr;
    CURLcode (*easy_getinfo)(CURL *, CURLINFO, ...) = nullptr;
    void (*easy_cleanup)(CURL *) = nullptr;
    const char *(*easy_strerror)(CURLcode) = nullptr;
    curl_slist *(*slist_append)(curl_slist *, const char *) = nullptr;
    void (*slist_free_all)(curl_slist *) = nullptr;
    CURLM *(*multi_init)() = nullptr;
    CURLMcode (*multi_add_handle)(CURLM *, CURL *) = nullptr;
    CURLMcode (*multi_remove_handle)(CURLM *, CURL *) = nullptr;
    CURLMcode (*multi_perform)(CURLM *, int *) = nullptr;
    CURLMcode (*multi_wait)(CURLM *, curl_waitfd *, unsigned int, int, int *) = nullptr;
    CURLMsg *(*multi_info_read)(CURLM *, int *) = nullptr;
    CURLMcode (*multi_cleanup)(CURLM *) = nullptr;
    curl_version_info_data *(*version_info)(CURLversion) = nullptr;
    // For WebSocket (CONNECT_ONLY): easy_perform 7.1, easy_send/easy_recv 7.18.
    CURLcode (*easy_perform)(CURL *) = nullptr;
    CURLcode (*easy_send)(CURL *, const void *, size_t, size_t *) = nullptr;
    CURLcode (*easy_recv)(CURL *, void *, size_t, size_t *) = nullptr;
    // Optional (cookie sharing, 7.10): null when missing; CurlOptions::cookies is then ignored.
    CURLSH *(*share_init)() = nullptr;
    CURLSHcode (*share_setopt)(CURLSH *, CURLSHoption, ...) = nullptr;
    CURLSHcode (*share_cleanup)(CURLSH *) = nullptr;
    std::string error;
    std::string version;
};

// Loads on first use; easy_init is null (and error set) if libcurl is unusable.
const CurlApi &api();

}  // namespace wnk::net
