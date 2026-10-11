// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/net/http_stream.hpp"

#include "wnk/log.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace wnk::net {
namespace {

constexpr const char *kCat = "http.stream";

bool parse_int(const std::string &s, size_t &i, int64_t *out) {
    const size_t start = i;
    int64_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (v > (INT64_MAX - 9) / 10) return false;
        v = v * 10 + (s[i] - '0');
        ++i;
    }
    *out = v;
    return i > start;
}

bool transient(NetError e) {
    switch (e) {
        case NetError::Connect:
        case NetError::Timeout:
        case NetError::Dns:
        case NetError::Io:
        case NetError::Protocol:  // reset or truncated mid-body
            return true;
        default:
            return false;
    }
}

}  // namespace

bool parse_content_range(const std::string &value, int64_t *first, int64_t *last, int64_t *total) {
    // RFC 9110 §14.4: "bytes" SP first "-" last "/" ( complete-length / "*" ). Unsatisfied ranges
    // ("bytes */1234") are not a valid range for a 206 and are rejected here.
    size_t i = 0;
    while (i < value.size() && value[i] == ' ') ++i;
    if (value.compare(i, 6, "bytes ") != 0) return false;
    i += 6;
    if (!parse_int(value, i, first) || i >= value.size() || value[i++] != '-') return false;
    if (!parse_int(value, i, last) || i >= value.size() || value[i++] != '/') return false;
    if (i < value.size() && value[i] == '*') {
        *total = -1;
        ++i;
    } else if (!parse_int(value, i, total)) {
        return false;
    }
    while (i < value.size() && value[i] == ' ') ++i;
    if (i != value.size() || *last < *first) return false;
    return *total < 0 || *last < *total;
}

struct HttpStream::Impl {
    std::shared_ptr<HttpTransport> transport;
    std::string url;
    HttpStreamOptions opt;

    mutable std::mutex m;
    std::condition_variable cv;
    std::thread worker;
    bool quit = false;
    bool need_fetch = false;  // the worker should start a request at write_pos
    uint64_t gen = 0;         // bumped by every restart (seek outside the buffer)
    std::shared_ptr<async::CancellationSource> transfer;  // of the request in flight

    // Ring buffer of file bytes [buf_start, write_pos); the byte at offset o lives at ring[o % cap].
    std::vector<uint8_t> ring;
    int64_t buf_start = 0, write_pos = 0, read_pos = 0;
    bool eof = false;       // the download reached the end of the resource at write_pos
    bool opened = false;    // the first response was accepted
    bool failed = false;    // gave up on this generation
    bool interrupted = false;
    int failures = 0;       // consecutive, reset by progress or a restart
    int64_t size = -1;
    bool seekable = false;
    NetError error = NetError::None;
    long status = 0;
    std::string detail;
    HttpStreamStats stats;

    size_t cap() const { return ring.size(); }

    void fail(NetError e, long http_status, std::string why) {
        failed = true;
        error = e;
        status = http_status;
        detail = std::move(why);
        WNK_LOGW(kCat, "%s: %s (%s)", url.c_str(), detail.c_str(), error_name(e));
        cv.notify_all();
    }

    // Starts over at `pos`: drops the buffer and the transfer in flight.
    void restart(int64_t pos) {
        ++gen;
        buf_start = write_pos = read_pos = pos;
        eof = failed = false;
        error = NetError::None;
        status = 0;
        detail.clear();
        failures = 0;
        need_fetch = true;
        if (transfer) transfer->cancel();
        cv.notify_all();
    }

    // How a response for a request at `off` is judged, on the worker thread with `m` held.
    struct Verdict {
        bool accept = false;
        bool at_end = false;     // 416: nothing at or after `off`
        bool retry = false;      // 502/503/504
        NetError error = NetError::None;
        long status = 0;
        std::string why;
    };

    Verdict judge(int64_t off, long st, const Headers &headers) {
        auto header = [&](const char *name) -> std::string {
            for (const auto &[k, v] : headers) {
                if (k == name) return v;
            }
            return {};
        };
        Verdict v;
        v.status = st;
        if (st == 206) {
            int64_t first = 0, last = 0, total = -1;
            if (!parse_content_range(header("content-range"), &first, &last, &total) || first != off) {
                v.error = NetError::Protocol;
                v.why = "206 with a Content-Range that does not start at " + std::to_string(off);
                return v;
            }
            if (opened && size >= 0 && total >= 0 && total != size) {
                v.error = NetError::Protocol;
                v.why = "the resource changed size (" + std::to_string(size) + " -> " + std::to_string(total) + ")";
                return v;
            }
            if (!opened) {
                seekable = true;
                size = total;
            }
            v.accept = true;
            return v;
        }
        if (st == 200) {
            if (off != 0) {
                v.error = NetError::Protocol;
                v.why = "the server ignored the byte range";
                return v;
            }
            if (!opened) {
                seekable = false;
                const std::string len = header("content-length");
                const std::string enc = header("content-encoding");
                size_t i = 0;
                int64_t n = -1;
                if ((enc.empty() || enc == "identity") && parse_int(len, i, &n) && i == len.size()) size = n;
            }
            v.accept = true;
            return v;
        }
        if (st == 416) {
            v.accept = true;
            v.at_end = true;
            return v;
        }
        v.error = NetError::Protocol;
        v.why = "HTTP " + std::to_string(st);
        v.retry = st == 502 || st == 503 || st == 504;
        return v;
    }

    // Copies a downloaded chunk into the ring, waiting for the reader to make room.
    bool store(uint64_t my_gen, const uint8_t *data, size_t n, std::unique_lock<std::mutex> &lock) {
        while (n > 0) {
            if (quit || gen != my_gen) return false;
            // Bytes more than keep_behind before the reader may be dropped.
            const int64_t floor = std::clamp<int64_t>(read_pos - static_cast<int64_t>(opt.keep_behind_bytes),
                                                      buf_start, write_pos);
            buf_start = floor;
            const size_t space = cap() - static_cast<size_t>(write_pos - buf_start);
            if (space == 0) {
                cv.wait(lock);
                continue;
            }
            size_t k = std::min(space, n);
            const size_t at = static_cast<size_t>(write_pos % static_cast<int64_t>(cap()));
            k = std::min(k, cap() - at);  // up to the end of the ring; the loop wraps
            std::memcpy(ring.data() + at, data, k);
            write_pos += static_cast<int64_t>(k);
            stats.bytes_received += static_cast<int64_t>(k);
            data += k;
            n -= k;
            failures = 0;
            cv.notify_all();
        }
        return true;
    }

    void run() {
        std::unique_lock<std::mutex> lock(m);
        for (;;) {
            cv.wait(lock, [&] { return quit || need_fetch; });
            if (quit) return;
            need_fetch = false;
            const uint64_t my_gen = gen;
            const int64_t off = write_pos;
            if (opened && size >= 0 && off >= size) {
                eof = true;
                cv.notify_all();
                continue;
            }
            auto source = std::make_shared<async::CancellationSource>();
            transfer = source;
            ++stats.requests;

            HttpRequest req;
            req.url = url;
            req.headers = opt.headers;
            req.headers.emplace_back("Range", "bytes=" + std::to_string(off) + "-");
            req.timeout_ms = opt.stall_timeout_ms;
            req.connect_timeout_ms = opt.connect_timeout_ms;
            req.max_redirects = opt.max_redirects;
            Verdict verdict;
            bool judged = false;
            HttpStreamSink sink;
            sink.on_response = [&](long st, const Headers &headers) {
                std::lock_guard<std::mutex> g(m);
                if (quit || gen != my_gen) return false;
                verdict = judge(off, st, headers);
                judged = true;
                if (verdict.accept && !opened) {
                    opened = true;
                    WNK_LOGI(kCat, "%s: %s, %lld bytes", url.c_str(), seekable ? "seekable" : "not seekable",
                             static_cast<long long>(size));
                    cv.notify_all();
                }
                return verdict.accept && !verdict.at_end;
            };
            sink.on_data = [&](const uint8_t *data, size_t n) {
                std::unique_lock<std::mutex> l(m);
                return store(my_gen, data, n, l);
            };

            lock.unlock();
            const HttpResponse r = transport->stream(req, source->token(), sink);
            lock.lock();
            transfer.reset();
            if (quit) return;
            if (gen != my_gen) continue;  // restarted; need_fetch is already set

            bool retry = false;
            if (judged && verdict.at_end) {
                eof = true;
                if (!opened) opened = true;
                if (size < 0) size = write_pos;
            } else if (judged && !verdict.accept) {
                if (verdict.retry) {
                    retry = true;
                    detail = verdict.why;
                } else {
                    fail(verdict.error, verdict.status, verdict.why);
                }
            } else if (r.error == NetError::None) {
                if (size >= 0 && write_pos < size) {
                    retry = true;  // closed early
                    detail = "body ended at " + std::to_string(write_pos) + " of " + std::to_string(size);
                } else {
                    eof = true;
                    if (size < 0) size = write_pos;
                }
            } else if (transient(r.error)) {
                retry = true;
                detail = r.detail;
            } else {
                fail(r.error, 0, r.detail.empty() ? error_name(r.error) : r.detail);
            }
            if (retry) {
                ++failures;
                if (opened && !seekable && write_pos > 0) {
                    fail(NetError::Io, 0, "connection lost and the server does not support byte ranges (" + detail + ")");
                } else if (failures > opt.max_retries) {
                    const NetError e = judged && !verdict.accept ? verdict.error
                                       : r.error != NetError::None ? r.error
                                                                   : NetError::Io;
                    fail(e, judged && !verdict.accept ? verdict.status : 0,
                         "gave up after " + std::to_string(failures) + " attempts: " + detail);
                } else {
                    const int backoff = std::min(3000, opt.retry_backoff_ms << std::min(failures - 1, 10));
                    WNK_LOGW(kCat, "%s: %s; reconnecting at %lld in %d ms", url.c_str(), detail.c_str(),
                             static_cast<long long>(write_pos), backoff);
                    cv.wait_for(lock, std::chrono::milliseconds(backoff), [&] { return quit || gen != my_gen; });
                    if (quit) return;
                    if (gen == my_gen) {
                        ++stats.reconnects;
                        need_fetch = true;
                    }
                }
            }
            cv.notify_all();
        }
    }
};

HttpStream::HttpStream(std::shared_ptr<HttpTransport> transport, std::string url, HttpStreamOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->transport = std::move(transport);
    impl_->url = std::move(url);
    impl_->opt = std::move(options);
    impl_->opt.buffer_bytes = std::max<size_t>(impl_->opt.buffer_bytes, 64u << 10);
}

HttpStream::~HttpStream() {
    Impl &s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.m);
        s.quit = true;
        if (s.transfer) s.transfer->cancel();
        s.cv.notify_all();
    }
    if (s.worker.joinable()) s.worker.join();
}

bool HttpStream::open() {
    Impl &s = *impl_;
    std::unique_lock<std::mutex> lock(s.m);
    if (!s.worker.joinable()) {
        std::string why;
        HttpRequest probe;
        probe.url = s.url;
        probe.headers = s.opt.headers;
        if (s.transport == nullptr || !validate_request(probe, &why)) {
            s.fail(s.transport == nullptr ? NetError::Unsupported : NetError::InvalidRequest, 0,
                   s.transport == nullptr ? "no HTTP transport" : why);
            return false;
        }
        s.ring.assign(s.opt.buffer_bytes + s.opt.keep_behind_bytes, 0);
        s.need_fetch = true;
        s.worker = std::thread([&s] { s.run(); });
    }
    s.cv.wait(lock, [&] { return s.opened || s.failed || s.interrupted; });
    if (s.opened) return true;
    if (s.interrupted && !s.failed) {
        s.error = NetError::Cancelled;
        s.detail = "interrupted";
    }
    return false;
}

int64_t HttpStream::size() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->size;
}

bool HttpStream::seekable() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->seekable;
}

int64_t HttpStream::position() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->read_pos;
}

int64_t HttpStream::read(void *dst, size_t n) {
    Impl &s = *impl_;
    std::unique_lock<std::mutex> lock(s.m);
    if (!s.opened) return -1;
    if (n == 0) return 0;
    for (;;) {
        if (s.interrupted) return -1;
        if (s.size >= 0 && s.read_pos >= s.size) return 0;
        if (s.read_pos >= s.buf_start && s.read_pos < s.write_pos) {
            const size_t cap = s.cap();
            size_t k = std::min(n, static_cast<size_t>(s.write_pos - s.read_pos));
            const size_t at = static_cast<size_t>(s.read_pos % static_cast<int64_t>(cap));
            const size_t first = std::min(k, cap - at);
            std::memcpy(dst, s.ring.data() + at, first);
            if (k > first) std::memcpy(static_cast<uint8_t *>(dst) + first, s.ring.data(), k - first);
            s.read_pos += static_cast<int64_t>(k);
            s.cv.notify_all();  // the worker may be waiting for room
            return static_cast<int64_t>(k);
        }
        if (s.eof) return 0;
        if (s.failed) return -1;
        s.cv.wait(lock);
    }
}

bool HttpStream::seek(int64_t pos) {
    Impl &s = *impl_;
    std::lock_guard<std::mutex> lock(s.m);
    if (pos < 0 || !s.opened) return false;
    if (pos >= s.buf_start && pos <= s.write_pos) {
        s.read_pos = pos;
        s.cv.notify_all();
        return true;
    }
    if (pos > s.write_pos && !s.eof && !s.failed && pos - s.write_pos <= static_cast<int64_t>(s.opt.seek_wait_bytes) &&
        (s.size < 0 || pos < s.size)) {
        s.read_pos = pos;  // the download gets there shortly
        s.cv.notify_all();
        return true;
    }
    if (!s.seekable) return false;
    s.restart(pos);
    return true;
}

void HttpStream::interrupt() {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->interrupted = true;
    impl_->cv.notify_all();
}

void HttpStream::reset_interrupt() {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->interrupted = false;
}

NetError HttpStream::error() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    if (impl_->interrupted && !impl_->failed) return NetError::Cancelled;
    return impl_->failed || !impl_->opened ? impl_->error : NetError::None;
}

long HttpStream::http_status() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->status;
}

std::string HttpStream::error_detail() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->detail;
}

HttpStreamStats HttpStream::stats() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->stats;
}

}  // namespace wnk::net
