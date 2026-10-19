// SPDX-License-Identifier: GPL-3.0-or-later
#include "curl_api.hpp"

#include <dlfcn.h>

#include <type_traits>

namespace wnk::net {

const CurlApi &api() {
    static CurlApi a = [] {
        CurlApi c;
        void *lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) {
            const char *e = dlerror();
            c.error = e != nullptr ? e : "dlopen libcurl.so.4 failed";
            return c;
        }
        bool ok = true;
        auto sym = [&](auto &fn, const char *name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            if (fn == nullptr) {
                ok = false;
                c.error += std::string(c.error.empty() ? "libcurl.so.4 lacks " : ", ") + name;
            }
        };
        sym(c.global_init, "curl_global_init");
        sym(c.easy_init, "curl_easy_init");
        sym(c.easy_setopt, "curl_easy_setopt");
        sym(c.easy_getinfo, "curl_easy_getinfo");
        sym(c.easy_cleanup, "curl_easy_cleanup");
        sym(c.easy_strerror, "curl_easy_strerror");
        sym(c.slist_append, "curl_slist_append");
        sym(c.slist_free_all, "curl_slist_free_all");
        sym(c.multi_init, "curl_multi_init");
        sym(c.multi_add_handle, "curl_multi_add_handle");
        sym(c.multi_remove_handle, "curl_multi_remove_handle");
        sym(c.multi_perform, "curl_multi_perform");
        sym(c.multi_wait, "curl_multi_wait");
        sym(c.multi_info_read, "curl_multi_info_read");
        sym(c.multi_cleanup, "curl_multi_cleanup");
        sym(c.version_info, "curl_version_info");
        sym(c.easy_perform, "curl_easy_perform");
        sym(c.easy_send, "curl_easy_send");
        sym(c.easy_recv, "curl_easy_recv");
        c.share_init = reinterpret_cast<decltype(c.share_init)>(dlsym(lib, "curl_share_init"));
        c.share_setopt = reinterpret_cast<decltype(c.share_setopt)>(dlsym(lib, "curl_share_setopt"));
        c.share_cleanup = reinterpret_cast<decltype(c.share_cleanup)>(dlsym(lib, "curl_share_cleanup"));
        if (!ok) {
            c.easy_init = nullptr;
            return c;
        }
        // curl_global_init is not thread-safe; done once here, before any worker uses curl.
        if (c.global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            c.error = "curl_global_init failed";
            c.easy_init = nullptr;
            return c;
        }
        if (const curl_version_info_data *v = c.version_info(CURLVERSION_NOW)) {
            c.version = std::string("libcurl/") + (v->version ? v->version : "?");
            if (v->ssl_version != nullptr) c.version += std::string(" ") + v->ssl_version;
        }
        return c;
    }();
    return a;
}


}  // namespace wnk::net
