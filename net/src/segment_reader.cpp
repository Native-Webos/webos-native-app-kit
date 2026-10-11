// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/net/segment_reader.hpp"

#include "wnk/log.hpp"
#include "wnk/net/http_stream.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace wnk::net {
namespace {

constexpr const char *kCat = "http.segments";

bool transient(NetError e) {
    switch (e) {
        case NetError::Connect:
        case NetError::Timeout:
        case NetError::Dns:
        case NetError::Io:
        case NetError::Protocol:
            return true;
        default:
            return false;
    }
}

std::string header_of(const Headers &headers, const char *name) {
    for (const auto &[k, v] : headers) {
        if (k == name) return v;
    }
    return {};
}

}  // namespace

struct SegmentReader::Impl {
    std::shared_ptr<HttpTransport> transport;
    std::vector<SegmentPart> parts;
    SegmentReaderOptions opt;

    mutable std::mutex m;
    std::condition_variable cv;
    std::thread worker;
    bool quit = false;
    async::CancellationSource stop;  // cancels the transfer in flight at destruction

    // Ring of stream bytes [read_pos, write_pos); byte o lives at ring[o % cap].
    std::vector<uint8_t> ring;
    int64_t read_pos = 0, write_pos = 0;
    bool done = false;    // every part is in
    bool failed = false;  // the download stopped at write_pos for good
    bool interrupted = false;
    size_t part = 0;      // being downloaded
    NetError error = NetError::None;
    long status = 0;
    std::string detail;
    SegmentReaderStats stats;

    void fail(NetError e, long st, std::string why) {
        failed = true;
        error = e;
        status = st;
        detail = std::move(why);
        WNK_LOGW(kCat, "part %zu of %zu: %s (%s)", part, parts.size(), detail.c_str(), error_name(e));
        cv.notify_all();
    }

    // Copies downloaded bytes into the ring, waiting for room. False when quitting.
    bool store(const uint8_t *data, size_t n) {
        std::unique_lock<std::mutex> lock(m);
        while (n > 0) {
            if (quit) return false;
            const size_t space = ring.size() - static_cast<size_t>(write_pos - read_pos);
            if (space == 0) {
                cv.wait(lock);
                continue;
            }
            const size_t at = static_cast<size_t>(write_pos % static_cast<int64_t>(ring.size()));
            const size_t k = std::min({space, n, ring.size() - at});
            std::memcpy(ring.data() + at, data, k);
            write_pos += static_cast<int64_t>(k);
            stats.bytes_received += static_cast<int64_t>(k);
            data += k;
            n -= k;
            cv.notify_all();
        }
        return true;
    }

    // Downloads one part; true when it is complete.
    bool fetch(const SegmentPart &p) {
        int64_t got = 0;  // bytes of this part delivered
        int failures = 0;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(m);
                if (quit) return false;
                ++stats.requests;
            }
            const int64_t off = p.offset + got;
            HttpRequest req;
            req.url = p.url;
            req.headers = opt.headers;
            const bool ranged = off > 0 || p.length >= 0;
            if (ranged) {
                std::string range = "bytes=" + std::to_string(off) + "-";
                if (p.length >= 0) range += std::to_string(p.offset + p.length - 1);
                req.headers.emplace_back("Range", range);
            }
            req.timeout_ms = opt.stall_timeout_ms;
            req.connect_timeout_ms = opt.connect_timeout_ms;
            req.max_redirects = opt.max_redirects;

            int64_t skip = 0;           // bytes to drop (a 200 answer to a range request)
            int64_t want = p.length >= 0 ? p.length - got : -1;  // bytes still wanted, -1 to the end
            bool complete = false;      // got everything wanted (the transfer may be cut short)
            long bad_status = 0;
            HttpStreamSink sink;
            sink.on_response = [&](long st, const Headers &headers) {
                if (st == 206) {
                    int64_t first = 0, last = 0, total = -1;
                    if (!parse_content_range(header_of(headers, "content-range"), &first, &last, &total) || first != off) {
                        bad_status = -1;
                        return false;
                    }
                    return true;
                }
                if (st == 200) {
                    skip = off;  // the server sent the whole resource
                    return true;
                }
                bad_status = st;
                return false;
            };
            sink.on_data = [&](const uint8_t *data, size_t n) {
                if (skip > 0) {
                    const size_t k = static_cast<size_t>(std::min<int64_t>(skip, static_cast<int64_t>(n)));
                    skip -= static_cast<int64_t>(k);
                    data += k;
                    n -= k;
                }
                if (want >= 0) n = static_cast<size_t>(std::min<int64_t>(want, static_cast<int64_t>(n)));
                if (n > 0) {
                    if (!store(data, n)) return false;
                    got += static_cast<int64_t>(n);
                    failures = 0;
                    if (want >= 0) want -= static_cast<int64_t>(n);
                }
                if (want == 0) {
                    complete = true;
                    return false;  // ends the transfer (a 200 for a range keeps sending)
                }
                return true;
            };
            const auto t0 = std::chrono::steady_clock::now();
            const HttpResponse r = transport->stream(req, stop.token(), sink);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            std::unique_lock<std::mutex> lock(m);
            stats.download_seconds += secs;
            if (quit) return false;
            if (complete || (r.error == NetError::None && bad_status == 0 && want < 0)) return true;
            std::string why;
            NetError e = NetError::Io;
            bool retry = false;
            if (bad_status == -1) {
                why = "206 with a Content-Range that does not start at " + std::to_string(off);
                e = NetError::Protocol;
            } else if (bad_status != 0) {
                why = "HTTP " + std::to_string(bad_status);
                e = NetError::Protocol;
                retry = bad_status == 502 || bad_status == 503 || bad_status == 504;
            } else if (r.error == NetError::None) {
                why = "body ended " + std::to_string(want) + " bytes early";
                retry = true;
            } else {
                why = r.detail.empty() ? error_name(r.error) : r.detail;
                e = r.error;
                retry = transient(r.error);
            }
            if (!retry || ++failures > opt.max_retries) {
                fail(e, bad_status > 0 ? bad_status : 0,
                     p.url + ": " + why + (retry ? " (gave up after " + std::to_string(failures) + " attempts)" : ""));
                return false;
            }
            const int backoff = std::min(3000, opt.retry_backoff_ms << std::min(failures - 1, 10));
            WNK_LOGW(kCat, "%s: %s; retrying at byte %lld in %d ms", p.url.c_str(), why.c_str(),
                     static_cast<long long>(off), backoff);
            ++stats.retries;
            cv.wait_for(lock, std::chrono::milliseconds(backoff), [&] { return quit; });
            if (quit) return false;
        }
    }

    void run() {
        for (size_t i = 0; i < parts.size(); ++i) {
            {
                std::lock_guard<std::mutex> lock(m);
                part = i;
            }
            if (!fetch(parts[i])) return;
            std::lock_guard<std::mutex> lock(m);
            ++stats.parts_done;
        }
        std::lock_guard<std::mutex> lock(m);
        part = parts.size();
        done = true;
        cv.notify_all();
    }
};

SegmentReader::SegmentReader(std::shared_ptr<HttpTransport> transport, std::vector<SegmentPart> parts,
                             SegmentReaderOptions options)
    : impl_(std::make_unique<Impl>()) {
    Impl &s = *impl_;
    s.transport = std::move(transport);
    s.parts = std::move(parts);
    s.opt = std::move(options);
    s.ring.assign(std::max<size_t>(s.opt.buffer_bytes, 64u << 10), 0);
    if (s.transport == nullptr) {
        s.fail(NetError::Unsupported, 0, "no HTTP transport");
        return;
    }
    s.worker = std::thread([&s] { s.run(); });
}

SegmentReader::~SegmentReader() {
    Impl &s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.m);
        s.quit = true;
        s.cv.notify_all();
    }
    s.stop.cancel();
    if (s.worker.joinable()) s.worker.join();
}

int64_t SegmentReader::read(void *dst, size_t n) {
    Impl &s = *impl_;
    std::unique_lock<std::mutex> lock(s.m);
    if (n == 0) return 0;
    for (;;) {
        if (s.interrupted) return -1;
        if (s.read_pos < s.write_pos) {
            const size_t cap = s.ring.size();
            const size_t k = std::min(n, static_cast<size_t>(s.write_pos - s.read_pos));
            const size_t at = static_cast<size_t>(s.read_pos % static_cast<int64_t>(cap));
            const size_t first = std::min(k, cap - at);
            std::memcpy(dst, s.ring.data() + at, first);
            if (k > first) std::memcpy(static_cast<uint8_t *>(dst) + first, s.ring.data(), k - first);
            s.read_pos += static_cast<int64_t>(k);
            s.cv.notify_all();
            return static_cast<int64_t>(k);
        }
        if (s.done) return 0;
        if (s.failed) return -1;
        s.cv.wait(lock);
    }
}

int64_t SegmentReader::position() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->read_pos;
}

void SegmentReader::interrupt() {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->interrupted = true;
    impl_->cv.notify_all();
}

void SegmentReader::reset_interrupt() {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->interrupted = false;
}

NetError SegmentReader::error() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    if (impl_->interrupted && !impl_->failed) return NetError::Cancelled;
    return impl_->failed ? impl_->error : NetError::None;
}

long SegmentReader::http_status() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->status;
}

std::string SegmentReader::error_detail() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->detail;
}

size_t SegmentReader::download_part() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->part;
}

SegmentReaderStats SegmentReader::stats() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->stats;
}

}  // namespace wnk::net
