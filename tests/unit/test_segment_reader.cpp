// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/net/segment_reader.hpp"

#include <atomic>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

using namespace wnk;
using namespace wnk::net;
using namespace std::chrono_literals;

namespace {

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> d(n);
    for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>((i * 31 + seed) & 0xFF);
    return d;
}

// Serves several resources by URL with "Range: bytes=a-b" / "bytes=a-" support, in chunks, honouring
// cancellation; scripted statuses and connection drops inject faults.
struct MultiTransport final : HttpTransport {
    std::map<std::string, std::vector<uint8_t>> files;
    std::atomic<bool> ranges{true};
    size_t chunk = 1000;
    std::mutex m;
    std::vector<std::string> requests;  // "url range"
    std::deque<long> statuses;
    std::deque<size_t> drop_after;
    std::chrono::milliseconds delay{0};

    std::string describe() const override { return "multi"; }

    HttpResponse perform(const HttpRequest &, const async::CancellationToken &) override {
        HttpResponse r;
        r.error = NetError::Unsupported;
        return r;
    }

    HttpResponse stream(const HttpRequest &req, const async::CancellationToken &token,
                        const HttpStreamSink &sink) override {
        std::string range;
        for (const auto &[k, v] : req.headers) {
            if (k == "Range") range = v;
        }
        long scripted = 0;
        size_t drop = SIZE_MAX;
        {
            std::lock_guard<std::mutex> lock(m);
            requests.push_back(req.url + " " + range);
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
        auto it = files.find(req.url);
        if (scripted == 0 && it == files.end()) scripted = 404;
        if (scripted != 0) {
            r.status = scripted;
            if (!sink.on_response(scripted, {})) r.error = NetError::Cancelled;
            return r;
        }
        const auto &data = it->second;
        const int64_t n = static_cast<int64_t>(data.size());
        int64_t first = 0, last = n - 1;
        if (!range.empty() && ranges) {
            first = std::atoll(range.c_str() + 6);
            const size_t dash = range.find('-');
            if (dash + 1 < range.size()) last = std::min<int64_t>(n - 1, std::atoll(range.c_str() + dash + 1));
        }
        Headers h;
        if (!range.empty() && ranges) {
            r.status = 206;
            h.emplace_back("content-range", "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" +
                                                std::to_string(n));
        } else {
            r.status = 200;
        }
        if (!sink.on_response(r.status, h)) {
            r.error = NetError::Cancelled;
            return r;
        }
        size_t sent = 0;
        for (int64_t at = first; at <= last;) {
            if (token.cancelled()) {
                r.error = NetError::Cancelled;
                return r;
            }
            if (sent >= drop) {
                r.error = NetError::Io;
                r.detail = "connection reset";
                return r;
            }
            if (delay.count() > 0) std::this_thread::sleep_for(delay);
            const size_t k = std::min({chunk, static_cast<size_t>(last + 1 - at), drop - sent});
            if (!sink.on_data(data.data() + at, k)) {
                r.error = NetError::Cancelled;
                return r;
            }
            at += static_cast<int64_t>(k);
            sent += k;
        }
        return r;
    }
};

std::vector<uint8_t> read_all(SegmentReader &r, int64_t *last_rc) {
    std::vector<uint8_t> out;
    uint8_t buf[777];
    for (;;) {
        const int64_t n = r.read(buf, sizeof buf);
        *last_rc = n;
        if (n <= 0) return out;
        out.insert(out.end(), buf, buf + n);
    }
}

std::vector<uint8_t> slice(const std::vector<uint8_t> &v, size_t off, size_t len) {
    return {v.begin() + static_cast<ptrdiff_t>(off), v.begin() + static_cast<ptrdiff_t>(off + len)};
}

}  // namespace

TEST("segment reader: init and media parts, whole files and byte ranges, read as one stream") {
    auto t = std::make_shared<MultiTransport>();
    t->files["https://h/init.mp4"] = pattern(500, 1);
    t->files["https://h/1.m4s"] = pattern(3000, 2);
    t->files["https://h/main.mp4"] = pattern(10000, 3);
    SegmentReader r(t, {{"https://h/init.mp4", 0, -1}, {"https://h/1.m4s", 0, -1}, {"https://h/main.mp4", 2000, 4000},
                        {"https://h/main.mp4", 6000, -1}});
    int64_t rc = 0;
    const auto got = read_all(r, &rc);
    CHECK_EQ(rc, int64_t{0});
    std::vector<uint8_t> want = t->files["https://h/init.mp4"];
    const auto one = t->files["https://h/1.m4s"];
    want.insert(want.end(), one.begin(), one.end());
    const auto mid = slice(t->files["https://h/main.mp4"], 2000, 4000);
    want.insert(want.end(), mid.begin(), mid.end());
    const auto tail = slice(t->files["https://h/main.mp4"], 6000, 4000);
    want.insert(want.end(), tail.begin(), tail.end());
    CHECK(got == want);
    CHECK_EQ(r.position(), static_cast<int64_t>(want.size()));
    CHECK_EQ(r.download_part(), size_t{4});
    const auto st = r.stats();
    CHECK_EQ(st.parts_done, 4);
    CHECK_EQ(st.requests, 4);
    std::lock_guard<std::mutex> lock(t->m);
    CHECK_EQ(t->requests[0], std::string("https://h/init.mp4 "));  // whole files go without a Range
    CHECK_EQ(t->requests[2], std::string("https://h/main.mp4 bytes=2000-5999"));
    CHECK_EQ(t->requests[3], std::string("https://h/main.mp4 bytes=6000-"));
}

TEST("segment reader: a dropped connection resumes at the first missing byte; 503 is retried") {
    auto t = std::make_shared<MultiTransport>();
    t->files["https://h/a"] = pattern(8000, 4);
    t->drop_after = {3000};
    SegmentReaderOptions o;
    o.retry_backoff_ms = 1;
    {
        SegmentReader r(t, {{"https://h/a", 1000, 6000}}, o);
        int64_t rc = 0;
        const auto got = read_all(r, &rc);
        CHECK_EQ(rc, int64_t{0});
        CHECK(got == slice(t->files["https://h/a"], 1000, 6000));
        CHECK_EQ(r.stats().retries, 1);
        std::lock_guard<std::mutex> lock(t->m);
        REQUIRE(t->requests.size() == 2);
        CHECK_EQ(t->requests[1], std::string("https://h/a bytes=4000-6999"));
    }
    t->statuses = {503, 503};
    SegmentReader r(t, {{"https://h/a", 0, -1}}, o);
    int64_t rc = 0;
    CHECK(read_all(r, &rc) == t->files["https://h/a"]);
    CHECK_EQ(r.stats().retries, 2);
}

TEST("segment reader: a server that ignores ranges is skipped up to the range and cut at its end") {
    auto t = std::make_shared<MultiTransport>();
    t->files["https://h/a"] = pattern(9000, 5);
    t->ranges = false;
    SegmentReader r(t, {{"https://h/a", 2500, 1500}, {"https://h/a", 7000, -1}});
    int64_t rc = 0;
    auto want = slice(t->files["https://h/a"], 2500, 1500);
    const auto tail = slice(t->files["https://h/a"], 7000, 2000);
    want.insert(want.end(), tail.begin(), tail.end());
    CHECK(read_all(r, &rc) == want);
    CHECK_EQ(rc, int64_t{0});
}

TEST("segment reader: a 404 ends the stream after what came before; retries are bounded") {
    auto t = std::make_shared<MultiTransport>();
    t->files["https://h/1"] = pattern(1200, 6);
    SegmentReader r(t, {{"https://h/1", 0, -1}, {"https://h/missing", 0, -1}, {"https://h/1", 0, -1}});
    int64_t rc = 0;
    CHECK(read_all(r, &rc) == t->files["https://h/1"]);
    CHECK_EQ(rc, int64_t{-1});
    CHECK(r.error() == NetError::Protocol);
    CHECK_EQ(r.http_status(), 404L);
    CHECK(r.error_detail().find("missing") != std::string::npos);

    t->statuses = {503, 503, 503, 503, 503};
    SegmentReaderOptions o;
    o.retry_backoff_ms = 1;
    o.max_retries = 2;
    SegmentReader g(t, {{"https://h/1", 0, -1}}, o);
    CHECK(read_all(g, &rc).empty());
    CHECK_EQ(rc, int64_t{-1});
    CHECK(g.error_detail().find("gave up") != std::string::npos);
}

TEST("segment reader: the buffer bounds read-ahead; interrupt unblocks; destruction mid-transfer is prompt") {
    auto t = std::make_shared<MultiTransport>();
    t->files["https://h/big"] = pattern(1u << 20, 7);
    SegmentReaderOptions o;
    o.buffer_bytes = 64u << 10;
    {
        SegmentReader r(t, {{"https://h/big", 0, -1}}, o);
        std::this_thread::sleep_for(50ms);
        CHECK(r.stats().bytes_received <= int64_t{64 << 10});  // waits for the reader
        uint8_t buf[100];
        CHECK_EQ(r.read(buf, sizeof buf), int64_t{100});
        r.interrupt();
        CHECK_EQ(r.read(buf, sizeof buf), int64_t{-1});
        CHECK(r.error() == NetError::Cancelled);
        r.reset_interrupt();
        CHECK(r.read(buf, sizeof buf) > 0);
    }  // destroyed while the worker waits for room
    t->delay = 20ms;
    const auto t0 = std::chrono::steady_clock::now();
    {
        SegmentReader r(t, {{"https://h/big", 0, -1}}, o);
        std::this_thread::sleep_for(30ms);
    }
    CHECK(std::chrono::steady_clock::now() - t0 < 500ms);
}

TEST("segment reader: parts are reported as they complete; stop_after_current ends at a part boundary") {
    auto t = std::make_shared<MultiTransport>();
    for (int i = 0; i < 6; ++i) t->files["https://h/" + std::to_string(i)] = pattern(20000, static_cast<uint8_t>(i));
    std::vector<SegmentPart> parts;
    for (int i = 0; i < 6; ++i) parts.push_back({"https://h/" + std::to_string(i), 0, -1});
    std::mutex m;
    std::vector<std::pair<size_t, int64_t>> done;
    SegmentReaderOptions o;
    o.buffer_bytes = 64u << 10;  // three parts fit; the fourth waits for the reader
    o.on_part_done = [&](size_t part, int64_t bytes, double seconds) {
        std::lock_guard<std::mutex> lock(m);
        done.emplace_back(part, bytes);
        CHECK(seconds >= 0);
    };
    SegmentReader r(t, parts, o);
    uint8_t buf[1000];
    CHECK(r.read(buf, sizeof buf) > 0);
    const size_t n = r.stop_after_current();
    CHECK(n >= 1 && n <= 6);
    int64_t rc = 0;
    const auto rest = read_all(r, &rc);
    CHECK_EQ(rc, int64_t{0});  // a clean end, not a failure
    CHECK_EQ(static_cast<int64_t>(rest.size()) + 1000, static_cast<int64_t>(n) * 20000);
    CHECK_EQ(r.stop_after_current(), n);  // idempotent once ended
    std::lock_guard<std::mutex> lock(m);
    CHECK_EQ(done.size(), n);
    for (size_t i = 0; i < done.size(); ++i) {
        CHECK_EQ(done[i].first, i);
        CHECK_EQ(done[i].second, int64_t{20000});
    }
}

TEST("segment reader: max_parts_ahead keeps the download a few parts ahead of the reader") {
    auto t = std::make_shared<MultiTransport>();
    std::vector<SegmentPart> parts;
    for (int i = 0; i < 8; ++i) {
        t->files["https://h/" + std::to_string(i)] = pattern(5000, static_cast<uint8_t>(i));
        parts.push_back({"https://h/" + std::to_string(i), 0, -1});
    }
    SegmentReaderOptions o;
    o.max_parts_ahead = 2;
    SegmentReader r(t, parts, o);
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(r.stats().parts_done, 3);  // the part being read (0) and two more
    uint8_t buf[5000];
    int64_t got = 0;
    while (got < 5000) got += r.read(buf, sizeof buf - static_cast<size_t>(got));  // part 0 read whole
    uint8_t one;
    CHECK_EQ(r.read(&one, 1), int64_t{1});  // now inside part 1
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(r.stats().parts_done, 4);
    int64_t rc = 0;
    CHECK_EQ(read_all(r, &rc).size(), size_t{8 * 5000 - 5001});
    CHECK_EQ(rc, int64_t{0});
}
