// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/net/http.hpp"

#include "wnk/log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <thread>

namespace wnk::net {

const char *error_name(NetError e) {
    switch (e) {
        case NetError::None: return "none";
        case NetError::InvalidRequest: return "invalid-request";
        case NetError::Unsupported: return "unsupported";
        case NetError::Dns: return "dns";
        case NetError::Connect: return "connect";
        case NetError::Tls: return "tls";
        case NetError::Timeout: return "timeout";
        case NetError::Cancelled: return "cancelled";
        case NetError::TooLarge: return "too-large";
        case NetError::Protocol: return "protocol";
        case NetError::Io: return "io";
    }
    return "?";
}

std::string HttpResponse::header(const std::string &name) const {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const auto &[k, v] : headers) {
        if (k == lower) return v;
    }
    return {};
}

bool is_idempotent_method(const std::string &method) {
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "PUT" || method == "DELETE";
}

bool validate_request(const HttpRequest &req, std::string *why) {
    auto fail = [&](const char *msg) {
        if (why != nullptr) *why = msg;
        return false;
    };
    const std::string &u = req.url;
    if (u.rfind("https://", 0) != 0 && u.rfind("http://", 0) != 0) return fail("URL must start with http:// or https://");
    if (u.find_first_of(" \r\n\t") != std::string::npos) return fail("URL contains whitespace");
    const size_t host_start = u.find("://") + 3;
    if (host_start >= u.size() || u[host_start] == '/' || u[host_start] == '?') return fail("URL has no host");
    if (req.method.empty()) return fail("empty method");
    for (char c : req.method) {
        if (!(std::isupper(static_cast<unsigned char>(c)) || c == '-')) return fail("method must be an upper-case token");
    }
    for (const auto &[k, v] : req.headers) {
        if (k.empty()) return fail("empty header name");
        for (char c : k) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return fail("bad header name");
        }
        if (v.find_first_of("\r\n") != std::string::npos) return fail("header value contains CR/LF");
    }
    if (req.timeout_ms <= 0 || req.connect_timeout_ms <= 0) return fail("timeouts must be positive");
    if (req.max_response_bytes == 0) return fail("max_response_bytes must be positive");
    if (req.max_redirects < 0) return fail("max_redirects must not be negative");
    return true;
}

HttpResponse HttpTransport::stream(const HttpRequest &req, const async::CancellationToken &token,
                                   const HttpStreamSink &sink) {
    HttpResponse r = perform(req, token);
    if (r.error != NetError::None) return r;
    std::string body = std::move(r.body);
    r.body.clear();
    if (!sink.on_response(r.status, r.headers) ||
        (!body.empty() && !sink.on_data(reinterpret_cast<const uint8_t *>(body.data()), body.size()))) {
        r.error = NetError::Cancelled;
        r.detail = "stopped by the receiver";
    }
    return r;
}

bool RetryPolicy::should_retry(const HttpResponse &r, int attempt) const {
    if (attempt >= max_attempts) return false;
    switch (r.error) {
        case NetError::Connect:
        case NetError::Timeout:
            return true;
        case NetError::Dns:
            return attempt == 1;  // one retry: resolvers on TVs can be slow right after wake-up
        case NetError::None:
            return r.status == 502 || r.status == 503 || r.status == 504;
        default:
            return false;  // TLS, cancellation, size limit, protocol errors will not fix themselves
    }
}

int RetryPolicy::backoff_ms(int attempt) const {
    const double d = initial_backoff_ms * std::pow(multiplier, std::max(0, attempt - 1));
    return static_cast<int>(std::min<double>(d, max_backoff_ms));
}

HttpClient::HttpClient(std::shared_ptr<HttpTransport> transport, async::WorkerPool &pool,
                       async::MainDispatcher &main, RetryPolicy retry)
    : transport_(std::move(transport)), pool_(pool), main_(main), retry_(retry) {}

HttpResponse HttpClient::perform_with_retry(const HttpRequest &req, const async::CancellationToken &token) {
    const bool idempotent = req.idempotent.value_or(is_idempotent_method(req.method));
    HttpResponse r;
    for (int attempt = 1;; ++attempt) {
        r = transport_->perform(req, token);
        r.attempts = attempt;
        WNK_LOGD("http", "%s %s -> %s %ld (attempt %d, %.0f ms)", req.method.c_str(), req.url.c_str(),
                 error_name(r.error), r.status, attempt, r.total_seconds * 1000);
        if (!idempotent || !retry_.should_retry(r, attempt) || token.cancelled()) return r;
        // Sleep in short slices so cancellation is still prompt during backoff.
        const auto until = async::Clock::now() + std::chrono::milliseconds(retry_.backoff_ms(attempt));
        while (async::Clock::now() < until) {
            if (token.cancelled()) {
                r.error = NetError::Cancelled;
                return r;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

bool HttpClient::send(HttpRequest req, async::CancellationToken token, async::Lifetime::Weak owner, Callback on_done) {
    std::string why;
    if (!validate_request(req, &why)) {
        WNK_LOGW("http", "rejected request: %s", why.c_str());
        return false;
    }
    auto &main = main_;
    return pool_.submit(
        [this, &main, req = std::move(req), owner, token, on_done = std::move(on_done)](
            const async::CancellationToken &t) mutable {
            auto response = std::make_shared<HttpResponse>(perform_with_retry(req, t));
            if (t.cancelled() || !owner.alive()) return;
            main.post([owner, token, response, on_done = std::move(on_done)]() {
                if (!owner.alive() || token.cancelled()) return;
                on_done(std::move(*response));
            });
        },
        token);
}

}  // namespace wnk::net
