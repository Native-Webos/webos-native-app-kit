// SPDX-License-Identifier: GPL-3.0-or-later
//
// HTTP(S) for TV apps (ADR-009).
//
//   HttpTransport   blocking, one request; run it on a worker thread. CurlTransport uses the system
//                   libcurl.so.4 loaded with dlopen (present on webOS 5.3.1+ and on Linux hosts), so a
//                   firmware without it reports Unsupported instead of failing to start.
//   HttpClient      asynchronous: runs requests on a WorkerPool, retries idempotent requests on
//                   transient failures, and delivers the result on the main thread through a
//                   MainDispatcher, unless the owner is gone or the request was cancelled.
//
// Rules this module enforces: TLS peer and host verification are always on; responses larger than
// max_response_bytes fail with TooLarge; only http/https URLs and redirects; URLs are redacted
// before logging and headers are never logged; non-idempotent requests are never retried.
#pragma once

#include "wnk/async/async.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wnk::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    Headers headers;
    std::string body;
    int timeout_ms = 15000;          // whole transfer
    int connect_timeout_ms = 5000;
    size_t max_response_bytes = 8u << 20;
    int max_redirects = 5;           // 0 disables following redirects
    // Retry only when the request is safe to repeat. Defaults to true for GET/HEAD/OPTIONS/PUT/DELETE
    // (see is_idempotent_method); set explicitly to override.
    std::optional<bool> idempotent;
};

enum class NetError {
    None,
    InvalidRequest,  // bad URL, scheme, method or header
    Unsupported,     // no usable HTTP stack on this device
    Dns,
    Connect,
    Tls,             // handshake or certificate verification failed
    Timeout,
    Cancelled,
    TooLarge,
    Protocol,        // malformed response, too many redirects, ...
    Io,
};

const char *error_name(NetError e);

struct HttpResponse {
    NetError error = NetError::None;
    long status = 0;                 // HTTP status when error == None
    Headers headers;                 // of the final response; names lower-cased
    std::string body;
    std::string effective_url;       // after redirects
    std::string detail;              // transport message for logs (never contains request headers)
    double total_seconds = 0;
    int attempts = 1;

    bool ok() const { return error == NetError::None && status >= 200 && status < 300; }
    // First header with this (case-insensitive) name, or empty.
    std::string header(const std::string &name) const;
};

bool is_idempotent_method(const std::string &method);

// Validates scheme (http/https), method token and header names/values (no CR/LF).
bool validate_request(const HttpRequest &req, std::string *why);

// Receives a response body as it arrives (HttpTransport::stream). Both callbacks run on the calling
// thread; returning false from either ends the transfer with NetError::Cancelled. on_data may block
// (that is how a reader applies backpressure), but then the token must be cancelled to unblock it.
struct HttpStreamSink {
    // Once, before any body bytes: status and lower-cased headers of the final response.
    std::function<bool(long status, const Headers &headers)> on_response;
    std::function<bool(const uint8_t *data, size_t size)> on_data;
};

class HttpTransport {
 public:
    virtual ~HttpTransport() = default;
    // Blocking. Must honour `token` promptly (within ~100 ms) and the request's limits.
    virtual HttpResponse perform(const HttpRequest &req, const async::CancellationToken &token) = 0;
    // Blocking, for media and other large bodies. Differs from perform(): the body goes to `sink`
    // instead of HttpResponse::body, max_response_bytes does not apply, timeout_ms is a stall limit
    // (no bytes for that long → Timeout) rather than a limit on the whole transfer, and no content
    // encoding is negotiated, so byte ranges stay exact. The default calls perform() and delivers the
    // buffered body, which is enough for test doubles.
    virtual HttpResponse stream(const HttpRequest &req, const async::CancellationToken &token,
                                const HttpStreamSink &sink);
    virtual std::string describe() const = 0;  // e.g. "libcurl/7.87.0 OpenSSL/1.1.1"
};

struct CurlOptions {
    std::string ca_bundle;   // extra CA file (PEM) used instead of the system store when set
    std::string user_agent = "wnk/0.1";
    // Keep the cookies servers set (Set-Cookie, also on redirects) in memory and send them back on
    // later requests of this transport, as a browser or ExoPlayer's Cronet source does: streaming
    // servers that set a cookie on the playlist and check it on segments and keys. Never logged, never
    // written to disk; gone with the transport. A request's own Cookie header is sent as well.
    bool cookies = false;
};

// nullptr with *error set if libcurl.so.4 cannot be loaded.
std::unique_ptr<HttpTransport> make_curl_transport(const CurlOptions &options = {}, std::string *error = nullptr);

// Retry policy for idempotent requests.
struct RetryPolicy {
    int max_attempts = 3;
    int initial_backoff_ms = 300;
    double multiplier = 2.0;
    int max_backoff_ms = 3000;
    // Transient outcomes worth retrying: Connect, Timeout, Dns (once), and HTTP 502/503/504.
    bool should_retry(const HttpResponse &r, int attempt) const;
    int backoff_ms(int attempt) const;  // delay before attempt+1
};

class HttpClient {
 public:
    HttpClient(std::shared_ptr<HttpTransport> transport, async::WorkerPool &pool, async::MainDispatcher &main,
               RetryPolicy retry = {});

    using Callback = std::function<void(HttpResponse)>;
    // Queues the request. `on_done` runs on the main thread (via `main`) unless `owner` is dead or
    // `token` was cancelled. Returns false if the pool is shutting down or the request is invalid
    // (in which case on_done is not called).
    bool send(HttpRequest req, async::CancellationToken token, async::Lifetime::Weak owner, Callback on_done);

    // Blocking variant with retries, for code already running on a worker thread.
    HttpResponse perform_with_retry(const HttpRequest &req, const async::CancellationToken &token);

 private:
    std::shared_ptr<HttpTransport> transport_;
    async::WorkerPool &pool_;
    async::MainDispatcher &main_;
    RetryPolicy retry_;
};

}  // namespace wnk::net
