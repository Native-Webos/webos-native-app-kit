// SPDX-License-Identifier: GPL-3.0-or-later
//
// HttpTransport over the system libcurl.so.4, loaded at runtime. Compiled against the vendored curl
// 7.87.0 headers (third_party/curl, identical to the NDK's); only long-standing API is used so the
// older curl in webOS 5/6 firmware works too (curl_multi_wait: 7.28; CURLOPT_PROTOCOLS: 7.19).
#include "wnk/net/http.hpp"

#include "curl_api.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <vector>

#include <dlfcn.h>

namespace wnk::net {
namespace {

struct Transfer {
    HttpResponse *response;
    size_t max_bytes;
    const CurlApi *api = nullptr;
    CURL *handle = nullptr;
    const HttpStreamSink *sink = nullptr;  // streaming: the body goes here instead of response->body
    bool too_large = false;
    bool responded = false;  // sink->on_response has been called
    bool stopped = false;    // the sink asked to stop

    bool respond() {
        responded = true;
        long status = 0;
        api->easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        if (!sink->on_response(status, response->headers)) stopped = true;
        return !stopped;
    }
};

size_t on_body(char *data, size_t size, size_t n, void *userp) {
    auto *t = static_cast<Transfer *>(userp);
    const size_t bytes = size * n;
    if (t->sink != nullptr) {
        if (!t->responded && !t->respond()) return 0;
        if (!t->sink->on_data(reinterpret_cast<const uint8_t *>(data), bytes)) {
            t->stopped = true;
            return 0;  // CURLE_WRITE_ERROR
        }
        return bytes;
    }
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

    ~CurlTransport() override {
        for (CURLM *m : idle_) api().multi_cleanup(m);
    }

    std::string describe() const override { return api().version; }

    HttpResponse perform(const HttpRequest &req, const async::CancellationToken &token) override {
        return run(req, token, nullptr);
    }

    HttpResponse stream(const HttpRequest &req, const async::CancellationToken &token,
                        const HttpStreamSink &sink) override {
        return run(req, token, &sink);
    }

 private:
    HttpResponse run(const HttpRequest &req, const async::CancellationToken &token, const HttpStreamSink *sink) {
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
        CURLM *m = take_multi();
        if (h == nullptr || m == nullptr) {
            if (h != nullptr) c.easy_cleanup(h);
            if (m != nullptr) c.multi_cleanup(m);
            r.error = NetError::Io;
            r.detail = "curl handle allocation failed";
            return r;
        }
        Transfer t{&r, req.max_response_bytes, &c, h, sink};
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
        if (sink == nullptr) {
            c.easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(req.timeout_ms));
        } else {
            // A stream may last for hours; give up only when nothing arrives for timeout_ms.
            c.easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
            c.easy_setopt(h, CURLOPT_LOW_SPEED_TIME, static_cast<long>(std::max(1, (req.timeout_ms + 999) / 1000)));
        }
        c.easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.connect_timeout_ms));
        c.easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        c.easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!options_.ca_bundle.empty()) c.easy_setopt(h, CURLOPT_CAINFO, options_.ca_bundle.c_str());
        c.easy_setopt(h, CURLOPT_USERAGENT, options_.user_agent.c_str());
        // Whatever this libcurl can decode; streams stay identity-encoded so byte ranges are exact.
        if (sink == nullptr) c.easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
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
        // An empty body never reaches on_body; the sink still gets the response.
        if (sink != nullptr && result == CURLE_OK && !cancelled && !t.responded) t.respond();
        r.error = map_error(result, t.too_large, cancelled || t.stopped);
        if (t.stopped) r.detail = "stopped by the receiver";
        if (r.error == NetError::None || t.stopped) {
            long status = 0;
            c.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
            r.status = status;
        }
        if (r.error != NetError::None) {
            r.body.clear();
            if (r.detail.empty()) r.detail = errbuf[0] != '\0' ? errbuf : c.easy_strerror(result);
        }
        char *effective = nullptr;
        if (c.easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective != nullptr) {
            r.effective_url = effective;
        }
        c.easy_getinfo(h, CURLINFO_TOTAL_TIME, &r.total_seconds);
        c.multi_remove_handle(m, h);
        give_multi(m);
        c.easy_cleanup(h);
        c.slist_free_all(headers);
        return r;
    }

    // libcurl keeps connections (and TLS sessions on them) in the multi handle. Reusing idle ones
    // saves a TCP and TLS handshake per request to the same server: segment after segment, about
    // two round trips each (measured ~650 ms of a ~1 s request to a server 320 ms away).
    CURLM *take_multi() {
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            if (!idle_.empty()) {
                CURLM *m = idle_.back();
                idle_.pop_back();
                return m;
            }
        }
        return api().multi_init();
    }

    void give_multi(CURLM *m) {
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            if (idle_.size() < kMaxIdle) {
                idle_.push_back(m);
                return;
            }
        }
        api().multi_cleanup(m);
    }

    static constexpr size_t kMaxIdle = 8;  // concurrent users: playback lanes, refresher, subtitles, UI
    std::mutex pool_mutex_;
    std::vector<CURLM *> idle_;  // most recently used last
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
