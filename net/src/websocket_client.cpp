// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/net/websocket.hpp"

#include "curl_api.hpp"
#include "wnk/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace wnk::net {
namespace {

constexpr const char *kCat = "ws";
using Clock = std::chrono::steady_clock;

struct ParsedUrl {
    bool secure = false;
    std::string host;      // as written (IPv6 keeps its brackets)
    std::string host_header;
    std::string path;      // path + query, at least "/"
    std::string curl_url;  // http(s)://host[:port]/
};

bool parse_ws_url(const std::string &url, ParsedUrl *out, std::string *why) {
    size_t rest;
    if (url.rfind("ws://", 0) == 0) {
        out->secure = false;
        rest = 5;
    } else if (url.rfind("wss://", 0) == 0) {
        out->secure = true;
        rest = 6;
    } else {
        *why = "not a ws:// or wss:// URL";
        return false;
    }
    const size_t slash = url.find_first_of("/?#", rest);
    std::string authority = url.substr(rest, slash == std::string::npos ? std::string::npos : slash - rest);
    if (authority.empty() || authority.find('@') != std::string::npos || authority.find_first_of(" \r\n") != std::string::npos) {
        *why = "invalid host in URL";
        return false;
    }
    std::string path = slash == std::string::npos ? "/" : url.substr(slash);
    const size_t hash = path.find('#');
    if (hash != std::string::npos) path = path.substr(0, hash);
    if (path.empty() || path[0] == '?') path = "/" + path;
    if (path.find_first_of(" \r\n") != std::string::npos) {
        *why = "invalid path in URL";
        return false;
    }
    out->host = authority;
    out->host_header = authority;  // includes a non-default port when one was written
    out->path = path;
    out->curl_url = std::string(out->secure ? "https://" : "http://") + authority + "/";
    return true;
}

int64_t ms_until(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t - Clock::now()).count();
}

}  // namespace

const char *ws_state_name(WsState s) {
    switch (s) {
        case WsState::Idle: return "idle";
        case WsState::Connecting: return "connecting";
        case WsState::Open: return "open";
        case WsState::Closing: return "closing";
        case WsState::Closed: return "closed";
    }
    return "?";
}

struct WebSocketClient::Impl {
    std::mutex mutex;
    std::deque<std::string> outbox;  // encoded frames
    int wake[2] = {-1, -1};
    std::atomic<bool> stop{false};
    std::atomic<bool> user_closed{false};
    bool close_requested = false;  // guarded by mutex
    int close_code = ws::kNormal;
    std::string close_reason;
    std::mt19937 rng{std::random_device{}()};

    Impl() {
        if (pipe(wake) == 0) {
            for (int fd : wake) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
        }
    }
    ~Impl() {
        for (int fd : wake) {
            if (fd >= 0) ::close(fd);
        }
    }
    void poke() {
        const char b = 1;
        if (wake[1] >= 0) (void)::write(wake[1], &b, 1);
    }
    void drain_wake() {
        char buf[64];
        while (wake[0] >= 0 && ::read(wake[0], buf, sizeof buf) > 0) {
        }
    }
    void random_bytes(uint8_t *out, size_t n) {
        std::lock_guard<std::mutex> lock(mutex);
        for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(rng() & 0xFF);
    }
    std::string frame(ws::Opcode op, std::string_view payload, bool fin = true) {
        uint8_t mask[4];
        random_bytes(mask, 4);
        return ws::encode_frame(op, payload, fin, mask);
    }
};

WebSocketClient::WebSocketClient(async::MainDispatcher &main, WebSocketOptions options)
    : main_(main), options_(std::move(options)), alive_(std::make_shared<int>(0)), impl_(std::make_unique<Impl>()) {}

WebSocketClient::~WebSocketClient() {
    if (thread_.joinable()) {
        // stop first: the thread may act on the close at once, and must already see it is a shutdown
        // (a short wait for the peer's answer, not close_timeout_ms).
        impl_->stop = true;  // also abandons a close handshake still waiting for the peer
        close(ws::kGoingAway, "client shutting down");
        impl_->poke();
        thread_.join();
    }
    alive_.reset();
}

void WebSocketClient::post(std::function<void()> fn) {
    std::weak_ptr<int> alive = alive_;
    main_.post([alive, fn = std::move(fn)] {
        if (!alive.expired()) fn();
    });
}

bool WebSocketClient::connect(const std::string &url, std::string *error) {
    ParsedUrl parsed;
    std::string why;
    if (!parse_ws_url(url, &parsed, &why)) {
        if (error != nullptr) *error = why;
        return false;
    }
    if (thread_.joinable()) {
        if (error != nullptr) *error = "already started";
        return false;
    }
    if (api().easy_init == nullptr || api().easy_send == nullptr) {
        if (error != nullptr) *error = "libcurl unavailable: " + api().error;
        return false;
    }
    state_ = WsState::Connecting;
    thread_ = std::thread([this, url] { run(url); });
    return true;
}

bool WebSocketClient::enqueue(ws::Opcode op, std::string payload) {
    const WsState s = state_.load();
    if (s == WsState::Closing || s == WsState::Closed || s == WsState::Idle) return false;
    std::string f = impl_->frame(op, payload);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->close_requested) return false;
        impl_->outbox.push_back(std::move(f));
    }
    impl_->poke();
    return true;
}

bool WebSocketClient::send_text(std::string text) {
    if (!ws::valid_utf8(text)) return false;
    return enqueue(ws::Opcode::Text, std::move(text));
}

bool WebSocketClient::send_binary(std::string bytes) { return enqueue(ws::Opcode::Binary, std::move(bytes)); }

void WebSocketClient::close(int code, std::string reason) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->close_requested) return;
        impl_->close_requested = true;
        impl_->close_code = code;
        impl_->close_reason = std::move(reason);
    }
    impl_->user_closed = true;
    impl_->poke();
}

namespace {

int xferinfo(void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<std::atomic<bool> *>(p)->load() ? 1 : 0;  // non-zero aborts the connect
}

// One connection: connect, handshake, run until it ends. Fills the outcome; returns an error text
// for failures before the connection opened (or a protocol error), empty otherwise.
struct Outcome {
    bool opened = false;
    bool stable = false;  // stayed open 5 s or delivered a message: resets the reconnect backoff
    bool clean = false;
    int code = ws::kAbnormal;
    std::string reason;
    std::string error;
};

}  // namespace

void WebSocketClient::run(std::string url) {
    const CurlApi &c = api();
    Impl &m = *impl_;
    ParsedUrl u;
    std::string why;
    parse_ws_url(url, &u, &why);
    int attempt = 0;

    while (!m.stop) {
        state_ = WsState::Connecting;
        Outcome out;
        CURL *curl = c.easy_init();
        // ---- connect (curl: DNS, TCP, TLS + verification) ----------------------------------------
        if (curl == nullptr) {
            out.error = "curl_easy_init failed";
        } else {
            c.easy_setopt(curl, CURLOPT_URL, u.curl_url.c_str());
            c.easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
            c.easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            c.easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options_.connect_timeout_ms));
            c.easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
            c.easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            c.easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &xferinfo);
            c.easy_setopt(curl, CURLOPT_XFERINFODATA, &m.stop);
            if (!options_.ca_bundle.empty()) c.easy_setopt(curl, CURLOPT_CAINFO, options_.ca_bundle.c_str());
            const CURLcode rc = c.easy_perform(curl);
            if (rc != CURLE_OK) out.error = std::string("connect failed: ") + c.easy_strerror(rc);
        }
        curl_socket_t sock = CURL_SOCKET_BAD;
        if (out.error.empty()) {
            c.easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);
            if (sock == CURL_SOCKET_BAD) out.error = "no socket from libcurl";
        }

        // Blocking-with-deadline send of a whole buffer.
        auto send_all = [&](const std::string &data, Clock::time_point deadline) -> bool {
            size_t off = 0;
            while (off < data.size()) {
                size_t n = 0;
                const CURLcode rc = c.easy_send(curl, data.data() + off, data.size() - off, &n);
                if (rc == CURLE_OK) {
                    off += n;
                    continue;
                }
                if (rc != CURLE_AGAIN) return false;
                const int64_t left = ms_until(deadline);
                if (left <= 0 || m.stop) return false;
                pollfd p{static_cast<int>(sock), POLLOUT, 0};
                ::poll(&p, 1, static_cast<int>(std::min<int64_t>(left, 200)));
            }
            return true;
        };

        ws::Decoder decoder(options_.max_message_bytes);
        std::string protocol;
        // ---- handshake --------------------------------------------------------------------------
        if (out.error.empty()) {
            uint8_t raw[16];
            m.random_bytes(raw, 16);
            const std::string key = ws::make_key(raw);
            std::string offered;
            for (size_t i = 0; i < options_.protocols.size(); ++i) offered += (i ? ", " : "") + options_.protocols[i];
            std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host_header +
                              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                              "\r\nSec-WebSocket-Version: 13\r\n";
            if (!offered.empty()) req += "Sec-WebSocket-Protocol: " + offered + "\r\n";
            for (const auto &[name, value] : options_.headers) {
                if (name.find_first_of(":\r\n") != std::string::npos || value.find_first_of("\r\n") != std::string::npos) continue;
                req += name + ": " + value + "\r\n";
            }
            req += "\r\n";
            const auto deadline = Clock::now() + std::chrono::milliseconds(options_.connect_timeout_ms);
            if (!send_all(req, deadline)) {
                out.error = "sending the handshake failed";
            } else {
                std::string head;
                char buf[4096];
                while (out.error.empty()) {
                    size_t n = 0;
                    const CURLcode rc = c.easy_recv(curl, buf, sizeof buf, &n);
                    if (rc == CURLE_OK && n == 0) {
                        out.error = "connection closed during the handshake";
                        break;
                    }
                    if (rc == CURLE_OK) {
                        head.append(buf, n);
                        const size_t end = head.find("\r\n\r\n");
                        if (end != std::string::npos) {
                            const std::string extra = head.substr(end + 4);
                            head.resize(end + 4);
                            if (!ws::check_handshake_response(head, key, offered, &protocol, &why)) out.error = why;
                            if (!extra.empty()) decoder.feed(extra.data(), extra.size());
                            break;
                        }
                        if (head.size() > 16384) out.error = "handshake response too large";
                        continue;
                    }
                    if (rc != CURLE_AGAIN) {
                        out.error = std::string("handshake: ") + c.easy_strerror(rc);
                        break;
                    }
                    const int64_t left = ms_until(deadline);
                    if (left <= 0 || m.stop) {
                        out.error = m.stop ? "stopped" : "handshake timed out";
                        break;
                    }
                    pollfd p{static_cast<int>(sock), POLLIN, 0};
                    ::poll(&p, 1, static_cast<int>(std::min<int64_t>(left, 200)));
                }
            }
        }

        // ---- open: frames until the connection ends ---------------------------------------------
        if (out.error.empty()) {
            out.opened = true;
            const auto opened_at = Clock::now();
            state_ = WsState::Open;
            WNK_LOGI(kCat, "open %s%s%s", url.c_str(), protocol.empty() ? "" : " protocol ", protocol.c_str());
            post([this, protocol] {
                if (on_open) on_open(protocol);
            });
            bool close_sent = false;
            auto last_rx = Clock::now();
            Clock::time_point pong_deadline{}, close_deadline{}, stop_cap{};
            bool waiting_pong = false;
            const auto io_deadline = [] { return Clock::now() + std::chrono::seconds(10); };
            auto send_close = [&](int code, const std::string &reason) {
                if (close_sent) return;
                close_sent = true;
                state_ = WsState::Closing;
                send_all(m.frame(ws::Opcode::Close, ws::close_payload(code, reason)), io_deadline());
                // Shutting down (destructor): wait at most 500 ms for the peer's answer.
                const int wait_ms = m.stop ? std::min(500, options_.close_timeout_ms) : options_.close_timeout_ms;
                close_deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
            };
            bool done = false;
            while (!done) {
                // Outgoing: queued messages, then a requested close.
                std::deque<std::string> frames;
                bool want_close = false;
                int want_code = ws::kNormal;
                std::string want_reason;
                {
                    std::lock_guard<std::mutex> lock(m.mutex);
                    frames.swap(m.outbox);
                    want_close = m.close_requested;
                    want_code = m.close_code;
                    want_reason = m.close_reason;
                }
                if (!close_sent) {
                    for (const auto &f : frames) {
                        if (!send_all(f, io_deadline())) {
                            out.error = "send failed";
                            done = true;
                            break;
                        }
                    }
                }
                if (done) break;
                if (m.stop && !close_sent) want_close = true, want_code = ws::kGoingAway;
                if (want_close && !close_sent) send_close(want_code, want_reason);
                if (m.stop && close_sent && options_.close_timeout_ms <= 0) break;
                if (m.stop && close_sent) {
                    // A close sent before the shutdown (the app's own close()) waits no longer either.
                    if (stop_cap == Clock::time_point{}) {
                        stop_cap = Clock::now() + std::chrono::milliseconds(std::min(500, options_.close_timeout_ms));
                    }
                    close_deadline = std::min(close_deadline, stop_cap);
                }

                // Incoming: read until libcurl has nothing buffered (TLS may hold data the socket no
                // longer signals), then decode.
                char buf[16384];
                bool eof = false;
                for (;;) {
                    size_t n = 0;
                    const CURLcode rc = c.easy_recv(curl, buf, sizeof buf, &n);
                    if (rc == CURLE_OK && n == 0) {
                        eof = true;
                        break;
                    }
                    if (rc == CURLE_OK) {
                        decoder.feed(buf, n);
                        last_rx = Clock::now();
                        continue;
                    }
                    if (rc != CURLE_AGAIN) eof = true;
                    break;
                }
                ws::Event e;
                while (!done && decoder.next(&e)) {
                    switch (e.kind) {
                        case ws::Event::Kind::Text:
                        case ws::Event::Kind::Binary: {
                            out.stable = true;
                            const bool binary = e.kind == ws::Event::Kind::Binary;
                            post([this, data = std::move(e.data), binary]() mutable {
                                if (on_message) on_message(std::move(data), binary);
                            });
                            break;
                        }
                        case ws::Event::Kind::Ping:
                            if (!close_sent) send_all(m.frame(ws::Opcode::Pong, e.data), io_deadline());
                            break;
                        case ws::Event::Kind::Pong: waiting_pong = false; break;
                        case ws::Event::Kind::Close:
                            // Answer with the same code (RFC 6455 §5.5.1), then the connection is done.
                            send_close(e.code == ws::kNoStatus ? ws::kNormal : e.code, "");
                            out.clean = true;
                            out.code = e.code;
                            out.reason = e.data;
                            done = true;
                            break;
                        case ws::Event::Kind::Error:
                            send_close(e.code, e.data);
                            out.error = "protocol error: " + e.data;
                            out.code = e.code;
                            done = true;
                            break;
                    }
                }
                if (done) break;
                if (Clock::now() - opened_at >= std::chrono::seconds(5)) out.stable = true;
                if (eof) {
                    if (close_sent) out.code = want_code;  // we closed; the server dropped instead of answering
                    break;
                }
                const auto now = Clock::now();
                if (close_sent && now >= close_deadline) break;
                if (waiting_pong && now >= pong_deadline) {
                    out.error = "no pong within " + std::to_string(options_.pong_timeout_ms) + " ms";
                    break;
                }
                if (!close_sent && options_.ping_interval_ms > 0 && !waiting_pong &&
                    now - last_rx >= std::chrono::milliseconds(options_.ping_interval_ms)) {
                    send_all(m.frame(ws::Opcode::Ping, "wnk"), io_deadline());
                    waiting_pong = true;
                    pong_deadline = now + std::chrono::milliseconds(options_.pong_timeout_ms);
                }
                // Sleep until data, a wake-up (send/close/stop) or the next timer.
                int64_t timeout = 1000;
                if (options_.ping_interval_ms > 0 && !waiting_pong) {
                    timeout = std::min<int64_t>(timeout, options_.ping_interval_ms - std::chrono::duration_cast<std::chrono::milliseconds>(now - last_rx).count());
                }
                if (waiting_pong) timeout = std::min<int64_t>(timeout, ms_until(pong_deadline));
                if (close_sent) timeout = std::min<int64_t>(timeout, ms_until(close_deadline));
                pollfd p[2] = {{static_cast<int>(sock), POLLIN, 0}, {m.wake[0], POLLIN, 0}};
                ::poll(p, 2, static_cast<int>(std::max<int64_t>(0, timeout)));
                if (p[1].revents & POLLIN) m.drain_wake();
            }
        }
        if (curl != nullptr) c.easy_cleanup(curl);

        // ---- report and decide whether to go again ----------------------------------------------
        if (!out.error.empty()) WNK_LOGW(kCat, "%s: %s", url.c_str(), out.error.c_str());
        if (out.opened) {
            WNK_LOGI(kCat, "closed %s: code %d%s%s (%s)", url.c_str(), out.code, out.reason.empty() ? "" : " ",
                     out.reason.c_str(), out.clean ? "clean" : "not clean");
        }
        if (!out.error.empty() && !(m.stop && out.error == "stopped")) {
            post([this, err = out.error] {
                if (on_error) on_error(err);
            });
        }
        if (out.opened) {
            post([this, code = out.code, reason = out.reason, clean = out.clean] {
                if (on_close) on_close(code, reason, clean);
            });
        }
        if (m.stop || m.user_closed || !options_.reconnect.enabled) break;
        // A server that accepts and drops at once must not get an endless fast retry loop.
        if (out.stable) attempt = 0;
        ++attempt;
        if (options_.reconnect.max_attempts > 0 && attempt > options_.reconnect.max_attempts) break;
        const double d = options_.reconnect.initial_ms * std::pow(options_.reconnect.multiplier, attempt - 1);
        const int delay = static_cast<int>(std::min<double>(d, options_.reconnect.max_ms));
        WNK_LOGI(kCat, "reconnecting to %s in %d ms (attempt %d)", url.c_str(), delay, attempt);
        post([this, attempt, delay] {
            if (on_reconnecting) on_reconnecting(attempt, delay);
        });
        state_ = WsState::Connecting;
        const auto until = Clock::now() + std::chrono::milliseconds(delay);
        while (!m.stop && !m.user_closed && Clock::now() < until) {
            pollfd p{m.wake[0], POLLIN, 0};
            ::poll(&p, 1, static_cast<int>(std::max<int64_t>(1, ms_until(until))));
            m.drain_wake();
        }
        {
            std::lock_guard<std::mutex> lock(m.mutex);
            m.outbox.clear();  // frames queued for the dead connection are not replayed
        }
    }
    state_ = WsState::Closed;
}

}  // namespace wnk::net
