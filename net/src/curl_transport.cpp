// SPDX-License-Identifier: GPL-3.0-or-later
//
// HttpTransport over the system libcurl.so.4, loaded at runtime. Compiled against the vendored curl
// 7.87.0 headers (third_party/curl, identical to the NDK's); only long-standing API is used so the
// older curl in webOS 5/6 firmware works too (curl_multi_wait: 7.28; CURLOPT_PROTOCOLS: 7.19).
#include "wnk/net/http.hpp"

// CURLOPT_PROTOCOLS is deprecated since curl 7.85 in favour of CURLOPT_PROTOCOLS_STR, which the older
// curl in webOS 5/6 firmware does not have. The numeric form keeps working in new curl too.
#define CURL_DISABLE_DEPRECATION 1
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

#include <dlfcn.h>

namespace wnk::net {
namespace {

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
    std::string error;
    std::string version;
};

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

struct Transfer {
    HttpResponse *response;
    size_t max_bytes;
    bool too_large = false;
};

size_t on_body(char *data, size_t size, size_t n, void *userp) {
    auto *t = static_cast<Transfer *>(userp);
    const size_t bytes = size * n;
    if (t->response->body.size() + bytes > t->max_bytes) {
        t->too_large = true;
        return 0;  // CURLE_WRITE_ERROR
    }
    t->response->body.append(data, bytes);
    return bytes;
}

size_t on_header(char *data, size_t size, size_t n, void *userp) {
    auto *t = static_cast<Transfer *>(userp);
    const size_t bytes = size * n;
    std::string line(data, bytes);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.rfind("HTTP/", 0) == 0) {
        t->response->headers.clear();  // a new response (redirect or 100-continue): keep only the last
        return bytes;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return bytes;
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    size_t v = colon + 1;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
    t->response->headers.emplace_back(std::move(name), line.substr(v));
    return bytes;
}

NetError map_error(CURLcode code, bool too_large, bool cancelled) {
    if (cancelled) return NetError::Cancelled;
    if (too_large) return NetError::TooLarge;
    switch (code) {
        case CURLE_OK: return NetError::None;
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_URL_MALFORMAT: return NetError::InvalidRequest;
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST: return NetError::Dns;
        case CURLE_COULDNT_CONNECT: return NetError::Connect;
        case CURLE_OPERATION_TIMEDOUT: return NetError::Timeout;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
        case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
        case CURLE_SSL_INVALIDCERTSTATUS:
        case CURLE_SSL_CRL_BADFILE:
        case CURLE_SSL_ENGINE_INITFAILED:
        case CURLE_USE_SSL_FAILED: return NetError::Tls;
        case CURLE_TOO_MANY_REDIRECTS:
        case CURLE_GOT_NOTHING:
        case CURLE_WEIRD_SERVER_REPLY:
        case CURLE_HTTP2:
        case CURLE_BAD_CONTENT_ENCODING:
        case CURLE_PARTIAL_FILE: return NetError::Protocol;
        case CURLE_ABORTED_BY_CALLBACK: return NetError::Cancelled;
        default: return NetError::Io;
    }
}

class CurlTransport final : public HttpTransport {
 public:
    explicit CurlTransport(CurlOptions options) : options_(std::move(options)) {}

    std::string describe() const override { return api().version; }

    HttpResponse perform(const HttpRequest &req, const async::CancellationToken &token) override {
        HttpResponse r;
        std::string why;
        if (!validate_request(req, &why)) {
            r.error = NetError::InvalidRequest;
            r.detail = why;
            return r;
        }
        const CurlApi &c = api();
        if (c.easy_init == nullptr) {
            r.error = NetError::Unsupported;
            r.detail = c.error;
            return r;
        }
        if (token.cancelled()) {
            r.error = NetError::Cancelled;
            return r;
        }
        CURL *h = c.easy_init();
        CURLM *m = c.multi_init();
        if (h == nullptr || m == nullptr) {
            if (h != nullptr) c.easy_cleanup(h);
            if (m != nullptr) c.multi_cleanup(m);
            r.error = NetError::Io;
            r.detail = "curl handle allocation failed";
            return r;
        }
        Transfer t{&r, req.max_response_bytes};
        char errbuf[CURL_ERROR_SIZE] = {};
        curl_slist *headers = nullptr;
        for (const auto &[k, v] : req.headers) headers = c.slist_append(headers, (k + ": " + v).c_str());
        headers = c.slist_append(headers, "Expect:");  // no 100-continue round trip for larger bodies

        const long protocols = CURLPROTO_HTTP | CURLPROTO_HTTPS;
        c.easy_setopt(h, CURLOPT_URL, req.url.c_str());
        c.easy_setopt(h, CURLOPT_NOSIGNAL, 1L);  // required with threads; timeouts without SIGALRM
        c.easy_setopt(h, CURLOPT_PROTOCOLS, protocols);
        c.easy_setopt(h, CURLOPT_REDIR_PROTOCOLS, protocols);
        c.easy_setopt(h, CURLOPT_FOLLOWLOCATION, req.max_redirects > 0 ? 1L : 0L);
        c.easy_setopt(h, CURLOPT_MAXREDIRS, static_cast<long>(req.max_redirects));
        c.easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(req.timeout_ms));
        c.easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.connect_timeout_ms));
        c.easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        c.easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!options_.ca_bundle.empty()) c.easy_setopt(h, CURLOPT_CAINFO, options_.ca_bundle.c_str());
        c.easy_setopt(h, CURLOPT_USERAGENT, options_.user_agent.c_str());
        c.easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // whatever this libcurl can decode
        c.easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
        c.easy_setopt(h, CURLOPT_WRITEFUNCTION, &on_body);
        c.easy_setopt(h, CURLOPT_WRITEDATA, &t);
        c.easy_setopt(h, CURLOPT_HEADERFUNCTION, &on_header);
        c.easy_setopt(h, CURLOPT_HEADERDATA, &t);
        c.easy_setopt(h, CURLOPT_HTTPHEADER, headers);
        if (req.method == "GET") {
            c.easy_setopt(h, CURLOPT_HTTPGET, 1L);
        } else if (req.method == "HEAD") {
            c.easy_setopt(h, CURLOPT_NOBODY, 1L);
        } else {
            if (req.method != "POST") c.easy_setopt(h, CURLOPT_CUSTOMREQUEST, req.method.c_str());
            if (!req.body.empty() || req.method == "POST") {
                c.easy_setopt(h, CURLOPT_POSTFIELDS, req.body.data());
                c.easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
            }
        }

        // Drive the transfer in short steps so cancellation is noticed within ~50 ms.
        c.multi_add_handle(m, h);
        bool cancelled = false;
        int running = 1;
        CURLcode result = CURLE_OK;
        while (running > 0) {
            if (c.multi_perform(m, &running) != CURLM_OK) {
                result = CURLE_FAILED_INIT;
                break;
            }
            if (token.cancelled()) {
                cancelled = true;
                break;
            }
            if (running > 0) {
                int fds = 0;
                c.multi_wait(m, nullptr, 0, 50, &fds);
            }
        }
        int left = 0;
        while (CURLMsg *msg = c.multi_info_read(m, &left)) {
            if (msg->msg == CURLMSG_DONE && msg->easy_handle == h) result = msg->data.result;
        }
        r.error = map_error(result, t.too_large, cancelled);
        if (r.error == NetError::None) {
            long status = 0;
            c.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
            r.status = status;
        } else {
            r.body.clear();
            r.detail = errbuf[0] != '\0' ? errbuf : c.easy_strerror(result);
        }
        char *effective = nullptr;
        if (c.easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective != nullptr) {
            r.effective_url = effective;
        }
        c.easy_getinfo(h, CURLINFO_TOTAL_TIME, &r.total_seconds);
        c.multi_remove_handle(m, h);
        c.multi_cleanup(m);
        c.easy_cleanup(h);
        c.slist_free_all(headers);
        return r;
    }

 private:
    CurlOptions options_;
};

}  // namespace

std::unique_ptr<HttpTransport> make_curl_transport(const CurlOptions &options, std::string *error) {
    if (api().easy_init == nullptr) {
        if (error != nullptr) *error = api().error;
        return nullptr;
    }
    return std::make_unique<CurlTransport>(options);
}

}  // namespace wnk::net
