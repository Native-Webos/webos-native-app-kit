// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/net/http_stream.hpp"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

using namespace wnk;
using namespace wnk::net;
using namespace std::chrono_literals;

namespace {

std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> d(n);
    for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>((i * 31 + i / 251) & 0xFF);
    return d;
}

// Serves one resource with optional byte-range support, in chunks, honouring cancellation, and
// injects faults: drops a connection after some bytes, or answers with a scripted status.
struct RangeTransport final : HttpTransport {
    std::vector<uint8_t> data;
    std::atomic<bool> ranges{true};
    size_t chunk = 4096;
    std::chrono::microseconds chunk_delay{0};
    std::mutex m;
    std::vector<int64_t> offsets;     // Range start of each request
    std::deque<long> statuses;        // replaces the next responses' status (with an empty body)
    std::deque<size_t> drop_after;    // the next connections fail after this many body bytes
    std::atomic<int> active{0};

    explicit RangeTransport(size_t n) : data(pattern(n)) {}

    HttpResponse perform(const HttpRequest &, const async::CancellationToken &) override {
        HttpResponse r;
        r.error = NetError::Unsupported;
        return r;
    }

    HttpResponse stream(const HttpRequest &req, const async::CancellationToken &token,
                        const HttpStreamSink &sink) override {
        ++active;
        HttpResponse r = serve(req, token, sink);
        --active;
        return r;
    }

    HttpResponse serve(const HttpRequest &req, const async::CancellationToken &token, const HttpStreamSink &sink) {
        int64_t off = 0;
        for (const auto &[k, v] : req.headers) {
            if (k == "Range") off = std::atoll(v.c_str() + 6);
        }
        long scripted = 0;
        size_t drop = SIZE_MAX;
        {
            std::lock_guard<std::mutex> lock(m);
            offsets.push_back(off);
            if (!statuses.empty()) {
                scripted = statuses.front();
                statuses.pop_front();
            }
            if (!drop_after.empty()) {
                drop = drop_after.front();
                drop_after.pop_front();
            }
        }
        HttpResponse r;
        const int64_t n = static_cast<int64_t>(data.size());
        if (scripted != 0) {
            r.status = scripted;
            if (!sink.on_response(scripted, {})) r.error = NetError::Cancelled;
            return r;
        }
        if (!ranges) off = 0;
        Headers h;
        if (ranges && off >= n) {
            r.status = 416;
            h.emplace_back("content-range", "bytes */" + std::to_string(n));
        } else if (ranges) {
            r.status = 206;
            h.emplace_back("content-range", "bytes " + std::to_string(off) + "-" + std::to_string(n - 1) + "/" +
                                                std::to_string(n));
        } else {
            r.status = 200;
            h.emplace_back("content-length", std::to_string(n));
        }
        if (!sink.on_response(r.status, h)) {
            r.error = NetError::Cancelled;
            return r;
        }
        if (r.status == 416) return r;
        size_t sent = 0;
        for (int64_t at = off; at < n;) {
            if (token.cancelled()) {
                r.error = NetError::Cancelled;
                return r;
            }
            if (sent >= drop) {
                r.error = NetError::Io;
                r.detail = "connection reset";
                return r;
            }
            const size_t k = std::min({chunk, static_cast<size_t>(n - at), drop - sent});
            if (chunk_delay.count() > 0) std::this_thread::sleep_for(chunk_delay);
            if (!sink.on_data(data.data() + at, k)) {
                r.error = NetError::Cancelled;
                return r;
            }
            at += static_cast<int64_t>(k);
            sent += k;
        }
        return r;
    }

    std::string describe() const override { return "range"; }

    std::vector<int64_t> requests() {
        std::lock_guard<std::mutex> lock(m);
        return offsets;
    }
};

HttpStreamOptions small(size_t buffer = 64u << 10) {
    HttpStreamOptions o;
    o.buffer_bytes = buffer;
    o.keep_behind_bytes = 16u << 10;
    o.seek_wait_bytes = 8u << 10;
    o.retry_backoff_ms = 1;
    return o;
}

// Reads everything from the current position to the end.
std::vector<uint8_t> read_all(HttpStream &s) {
    std::vector<uint8_t> out;
    uint8_t buf[7000];
    for (;;) {
        const int64_t n = s.read(buf, sizeof buf);
        if (n <= 0) break;
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

// Reads exactly n bytes unless the stream ends or fails first (read() may return fewer).
size_t read_exact(HttpStream &s, uint8_t *dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        const int64_t k = s.read(dst + got, n - got);
        if (k <= 0) break;
        got += static_cast<size_t>(k);
    }
    return got;
}

bool same(const std::vector<uint8_t> &got, const std::vector<uint8_t> &all, size_t from) {
    return got.size() == all.size() - from && std::memcmp(got.data(), all.data() + from, got.size()) == 0;
}

}  // namespace

TEST("http stream: Content-Range parsing") {
    int64_t a = 0, b = 0, t = 0;
    CHECK(parse_content_range("bytes 0-99/1234", &a, &b, &t));
    CHECK_EQ(a, 0);
    CHECK_EQ(b, 99);
    CHECK_EQ(t, 1234);
    CHECK(parse_content_range("bytes 100-199/*", &a, &b, &t));
    CHECK_EQ(t, -1);
    CHECK(!parse_content_range("bytes */1234", &a, &b, &t));
    CHECK(!parse_content_range("bytes 5-4/10", &a, &b, &t));
    CHECK(!parse_content_range("bytes 0-10/10", &a, &b, &t));
    CHECK(!parse_content_range("items 0-1/2", &a, &b, &t));
    CHECK(!parse_content_range("bytes 0-1/2x", &a, &b, &t));
    CHECK(!parse_content_range("", &a, &b, &t));
}

TEST("http stream: reads a whole resource through a buffer smaller than it") {
    auto t = std::make_shared<RangeTransport>(300000);
    HttpStream s(t, "http://test/file", small());
    REQUIRE(s.open());
    CHECK(s.seekable());
    CHECK_EQ(s.size(), 300000);
    CHECK(same(read_all(s), t->data, 0));
    CHECK_EQ(s.position(), 300000);
    CHECK_EQ(s.stats().requests, 1);
    uint8_t b;
    CHECK_EQ(s.read(&b, 1), 0);
}

TEST("http stream: seeks inside the buffer reuse it; far seeks send a new range request") {
    auto t = std::make_shared<RangeTransport>(500000);
    HttpStream s(t, "http://test/file", small());
    REQUIRE(s.open());
    uint8_t buf[1000];
    REQUIRE(s.read(buf, sizeof buf) > 0);
    // Short backward seek (kept behind) and a short forward one (waits for the download).
    REQUIRE(s.seek(10));
    CHECK_EQ(read_exact(s, buf, 10), 10u);
    CHECK(std::memcmp(buf, t->data.data() + 10, 10) == 0);
    REQUIRE(s.seek(4000));
    REQUIRE(read_exact(s, buf, 100) == 100u);
    CHECK(std::memcmp(buf, t->data.data() + 4000, 100) == 0);
    CHECK_EQ(t->requests().size(), 1u);
    // Far ahead (the MP4 index at the end) and back to the start.
    REQUIRE(s.seek(450000));
    CHECK(same(read_all(s), t->data, 450000));
    REQUIRE(s.seek(0));
    CHECK(same(read_all(s), t->data, 0));
    const auto offs = t->requests();
    REQUIRE(offs.size() == 3u);
    CHECK_EQ(offs[1], 450000);
    CHECK_EQ(offs[2], 0);
}

TEST("http stream: seeking to or past the end reads nothing and sends no request") {
    auto t = std::make_shared<RangeTransport>(100000);
    HttpStream s(t, "http://test/file", small());
    REQUIRE(s.open());
    REQUIRE(s.seek(100000));
    uint8_t b;
    CHECK_EQ(s.read(&b, 1), 0);
    REQUIRE(s.seek(200000));
    CHECK_EQ(s.read(&b, 1), 0);
    CHECK_EQ(t->requests().size(), 1u);
    CHECK(s.seek(99999));
    CHECK_EQ(s.read(&b, 1), 1);
    CHECK_EQ(b, t->data[99999]);
}

TEST("http stream: a dropped connection resumes from the first missing byte") {
    auto t = std::make_shared<RangeTransport>(200000);
    t->drop_after = {50000, 30000};
    HttpStream s(t, "http://test/file", small());
    REQUIRE(s.open());
    CHECK(same(read_all(s), t->data, 0));
    const auto offs = t->requests();
    REQUIRE(offs.size() == 3u);
    CHECK_EQ(offs[1], 50000);
    CHECK_EQ(offs[2], 80000);
    CHECK_EQ(s.stats().reconnects, 2);
}

TEST("http stream: gives up after max_retries failures in a row") {
    auto t = std::make_shared<RangeTransport>(200000);
    t->drop_after = {10000, 0, 0, 0, 0};
    auto o = small();
    o.max_retries = 3;
    HttpStream s(t, "http://test/file", o);
    REQUIRE(s.open());
    const auto got = read_all(s);
    CHECK_EQ(got.size(), 10000u);
    uint8_t b;
    CHECK_EQ(s.read(&b, 1), -1);
    CHECK(s.error() == NetError::Io);
    CHECK(s.error_detail().find("gave up after 4 attempts") != std::string::npos);
    CHECK_EQ(t->requests().size(), 4u);
    // A seek starts over and clears the failure.
    REQUIRE(s.seek(150000));
    CHECK(same(read_all(s), t->data, 150000));
}

TEST("http stream: 503 is retried, 404 fails at once with the status") {
    auto t = std::make_shared<RangeTransport>(10000);
    t->statuses = {503};
    HttpStream s(t, "http://test/file", small());
    REQUIRE(s.open());
    CHECK(same(read_all(s), t->data, 0));

    auto t2 = std::make_shared<RangeTransport>(10000);
    t2->statuses = {404};
    HttpStream s2(t2, "http://test/missing", small());
    CHECK(!s2.open());
    CHECK(s2.error() == NetError::Protocol);
    CHECK_EQ(s2.http_status(), 404);
    CHECK_EQ(t2->requests().size(), 1u);
}

TEST("http stream: a server without ranges reads forward only") {
    auto t = std::make_shared<RangeTransport>(100000);
    t->ranges = false;
    HttpStream s(t, "http://test/file", small());
    REQUIRE(s.open());
    CHECK(!s.seekable());
    CHECK_EQ(s.size(), 100000);
    uint8_t buf[100];
    REQUIRE(read_exact(s, buf, sizeof buf) == 100u);
    CHECK(s.seek(50));          // still buffered
    CHECK(!s.seek(99000));      // would need a range request
    CHECK_EQ(s.position(), 50);
    CHECK(same(read_all(s), t->data, 50));
}

TEST("http stream: a reconnect to a server that stopped honouring ranges fails") {
    auto t = std::make_shared<RangeTransport>(100000);
    t->drop_after = {20000};
    HttpStream s(t, "http://test/file", small());
    REQUIRE(s.open());
    t->ranges = false;
    const auto got = read_all(s);
    CHECK_EQ(got.size(), 20000u);
    CHECK(s.error() == NetError::Protocol);
    CHECK(s.error_detail().find("ignored the byte range") != std::string::npos);
}

TEST("http stream: the download stops at buffer_bytes ahead of the reader") {
    auto t = std::make_shared<RangeTransport>(1u << 20);
    HttpStream s(t, "http://test/file", small(64u << 10));
    REQUIRE(s.open());
    std::this_thread::sleep_for(100ms);
    const int64_t received = s.stats().bytes_received;
    CHECK(received <= (64 << 10) + (16 << 10));
    CHECK(received >= 64 << 10);
    uint8_t buf[32768];
    REQUIRE(s.read(buf, sizeof buf) > 0);
}

TEST("http stream: interrupt wakes a blocked read; destruction cancels the transfer") {
    auto t = std::make_shared<RangeTransport>(1u << 20);
    t->chunk_delay = 200ms;
    auto s = std::make_unique<HttpStream>(t, "http://test/file", small());
    REQUIRE(s->open());
    std::vector<uint8_t> buf(1u << 20);
    // Everything before the first chunk arrives: blocks until interrupted.
    std::thread stopper([&] {
        std::this_thread::sleep_for(50ms);
        s->interrupt();
    });
    const auto t0 = async::Clock::now();
    CHECK_EQ(s->read(buf.data(), buf.size()), -1);
    CHECK(s->error() == NetError::Cancelled);
    CHECK(async::Clock::now() - t0 < 1s);
    stopper.join();
    s->reset_interrupt();
    CHECK(s->read(buf.data(), 10) > 0);
    s.reset();
    CHECK_EQ(t->active.load(), 0);
}

TEST("http stream: invalid URL and missing transport fail open") {
    HttpStream a(std::make_shared<RangeTransport>(10), "ftp://x/y", small());
    CHECK(!a.open());
    CHECK(a.error() == NetError::InvalidRequest);
    HttpStream b(nullptr, "http://x/y", small());
    CHECK(!b.open());
    CHECK(b.error() == NetError::Unsupported);
}
