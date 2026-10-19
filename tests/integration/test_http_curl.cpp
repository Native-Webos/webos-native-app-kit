// SPDX-License-Identifier: GPL-3.0-or-later
//
// The curl transport against a local HTTP/HTTPS server (tests/integration/http_server.py starts it and
// passes its addresses through the environment). Real sockets, real TLS, nothing leaves 127.0.0.1.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/net/http.hpp"
#include "wnk/net/http_stream.hpp"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace wnk;
using namespace wnk::net;
using namespace std::chrono_literals;

namespace {

std::string env(const char *name) {
    const char *v = std::getenv(name);
    return v != nullptr ? v : "";
}

const std::string kHttp = env("WNK_TEST_HTTP_BASE");
const std::string kHttps = env("WNK_TEST_HTTPS_BASE");
const std::string kHttpsIp = env("WNK_TEST_HTTPS_IP");
const std::string kCa = env("WNK_TEST_CA");

std::unique_ptr<HttpTransport> transport(bool with_ca = false) {
    CurlOptions o;
    if (with_ca) o.ca_bundle = kCa;
    std::string err;
    auto t = make_curl_transport(o, &err);
    if (!t) std::fprintf(stderr, "  curl unavailable: %s\n", err.c_str());
    return t;
}

HttpRequest get(const std::string &url) {
    HttpRequest r;
    r.url = url;
    return r;
}

#define NEED_SERVER()                                                  \
    do {                                                               \
        if (kHttp.empty()) {                                           \
            std::printf("  SKIP (run through http_server.py)\n");      \
            return;                                                    \
        }                                                              \
    } while (0)

}  // namespace

TEST("plain GET returns status, body and headers") {
    NEED_SERVER();
    auto t = transport();
    REQUIRE(t);
    HttpResponse r = t->perform(get(kHttp + "/ok"), {});
    CHECK(r.error == NetError::None);
    CHECK_EQ(r.status, 200L);
    CHECK_EQ(r.body, std::string("hello"));
    CHECK_EQ(r.header("X-Server"), std::string("wnk-test"));
    CHECK(r.effective_url.find("/ok") != std::string::npos);
    CHECK(t->describe().rfind("libcurl/", 0) == 0);
}

TEST("HTTP error statuses are responses, not transport errors") {
    NEED_SERVER();
    auto t = transport();
    REQUIRE(t);
    HttpResponse r = t->perform(get(kHttp + "/status/404"), {});
    CHECK(r.error == NetError::None);
    CHECK_EQ(r.status, 404L);
    CHECK(!r.ok());
}

TEST("responses larger than the limit fail with TooLarge") {
    NEED_SERVER();
    auto t = transport();
    REQUIRE(t);
    HttpRequest req = get(kHttp + "/big?n=100000");
    req.max_response_bytes = 10000;
    HttpResponse r = t->perform(req, {});
    CHECK(r.error == NetError::TooLarge);
    CHECK(r.body.empty());
    req.max_response_bytes = 100000;
    CHECK(t->perform(req, {}).error == NetError::None);
}

TEST("timeouts and cancellation are prompt") {
    NEED_SERVER();
    auto t = transport();
    REQUIRE(t);
    HttpRequest slow = get(kHttp + "/slow?ms=3000");
    slow.timeout_ms = 300;
    auto t0 = async::Clock::now();
    CHECK(t->perform(slow, {}).error == NetError::Timeout);
    CHECK(async::Clock::now() - t0 < 1500ms);

    slow.timeout_ms = 10000;
    async::CancellationSource src;
    std::thread canceller([&] {
        std::this_thread::sleep_for(150ms);
        src.cancel();
    });
    t0 = async::Clock::now();
    HttpResponse r = t->perform(slow, src.token());
    canceller.join();
    CHECK(r.error == NetError::Cancelled);
    CHECK(async::Clock::now() - t0 < 600ms);
}

TEST("redirects are followed unless disabled") {
    NEED_SERVER();
    auto t = transport();
    REQUIRE(t);
    HttpResponse r = t->perform(get(kHttp + "/redirect"), {});
    CHECK_EQ(r.status, 200L);
    CHECK_EQ(r.body, std::string("hello"));
    HttpRequest no = get(kHttp + "/redirect");
    no.max_redirects = 0;
    CHECK_EQ(t->perform(no, {}).status, 302L);
}

TEST("cookies: kept across requests (also when set on a redirect) only when the transport asks") {
    NEED_SERVER();
    CurlOptions o;
    o.cookies = true;
    auto jar = make_curl_transport(o);
    REQUIRE(jar);
    HttpResponse r = jar->perform(get(kHttp + "/cookie/set"), {});
    CHECK_EQ(r.status, 200L);  // the redirect's own Set-Cookie is sent to its target
    CHECK_EQ(r.body, std::string("cookie ok"));
    CHECK_EQ(jar->perform(get(kHttp + "/cookie/check"), {}).status, 200L);  // and to later requests
    auto plain = transport();
    REQUIRE(plain);
    CHECK_EQ(plain->perform(get(kHttp + "/cookie/set"), {}).status, 403L);
    CHECK_EQ(plain->perform(get(kHttp + "/cookie/check"), {}).status, 403L);
    HttpRequest own = get(kHttp + "/cookie/check");
    own.headers.emplace_back("Cookie", "session=abc123");  // a request's own Cookie header still works
    CHECK_EQ(plain->perform(own, {}).status, 200L);
    auto fresh = make_curl_transport(o);  // each transport has its own cookies
    CHECK_EQ(fresh->perform(get(kHttp + "/cookie/check"), {}).status, 403L);
}

TEST("cookies: a request's own Cookie header is sent with the kept ones; streams use them too") {
    NEED_SERVER();
    CurlOptions o;
    o.cookies = true;
    auto jar = make_curl_transport(o);
    REQUIRE(jar);
    CHECK_EQ(jar->perform(get(kHttp + "/cookie/set"), {}).status, 200L);
    HttpRequest own = get(kHttp + "/echo");
    own.headers.emplace_back("Cookie", "extra=1");
    const std::string echoed = jar->perform(own, {}).body;
    CHECK(echoed.find("extra=1") != std::string::npos);
    CHECK(echoed.find("session=abc123") != std::string::npos);
    long status = 0;
    std::string body;
    HttpStreamSink sink;
    sink.on_response = [&](long st, const Headers &) {
        status = st;
        return true;
    };
    sink.on_data = [&](const uint8_t *d, size_t n) {
        body.append(reinterpret_cast<const char *>(d), n);
        return true;
    };
    CHECK(jar->stream(get(kHttp + "/cookie/check"), {}, sink).error == NetError::None);
    CHECK_EQ(status, 200L);
    CHECK_EQ(body, std::string("cookie ok"));
}

TEST("cookies: one transport shared by threads keeps one cookie store") {
    NEED_SERVER();
    CurlOptions o;
    o.cookies = true;
    std::shared_ptr<HttpTransport> jar = make_curl_transport(o);
    REQUIRE(jar);
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; i++) {
        threads.emplace_back([&] {
            for (int k = 0; k < 10; k++) {
                if (jar->perform(get(kHttp + "/cookie/set"), {}).status == 200) ++ok;
                if (jar->perform(get(kHttp + "/cookie/check"), {}).status == 200) ++ok;
            }
        });
    }
    for (std::thread &t : threads) t.join();
    CHECK_EQ(ok.load(), 120);
}

TEST("POST, PUT and custom headers reach the server; no Expect header") {
    NEED_SERVER();
    auto t = transport();
    REQUIRE(t);
    HttpRequest post = get(kHttp + "/echo");
    post.method = "POST";
    post.body = std::string(2000, 'b');  // big enough that curl would add Expect: 100-continue
    post.headers = {{"X-Test", "yes"}, {"Content-Type", "text/plain"}};
    HttpResponse r = t->perform(post, {});
    CHECK_EQ(r.status, 200L);
    CHECK(r.body.find("\"method\": \"POST\"") != std::string::npos);
    CHECK(r.body.find("\"x_test\": \"yes\"") != std::string::npos);
    CHECK(r.body.find("\"expect\": \"\"") != std::string::npos);
    CHECK(r.body.find(std::string(2000, 'b')) != std::string::npos);
    HttpRequest put = get(kHttp + "/echo");
    put.method = "PUT";
    put.body = "v";
    CHECK(t->perform(put, {}).body.find("\"method\": \"PUT\"") != std::string::npos);
}

TEST("gzip responses are decoded") {
    NEED_SERVER();
    auto t = transport();
    REQUIRE(t);
    HttpResponse r = t->perform(get(kHttp + "/gzip"), {});
    CHECK_EQ(r.status, 200L);
    CHECK(r.body.rfind("compressed body compressed body", 0) == 0);
}

TEST("TLS: trusted CA works, unknown CA and wrong host name fail") {
    NEED_SERVER();
    auto trusted = transport(true);
    REQUIRE(trusted);
    HttpResponse ok = trusted->perform(get(kHttps + "/ok"), {});
    CHECK(ok.error == NetError::None);
    CHECK_EQ(ok.body, std::string("hello"));
    auto system = transport(false);  // system store does not know the test CA
    HttpResponse bad = system->perform(get(kHttps + "/ok"), {});
    CHECK(bad.error == NetError::Tls);
    CHECK(!bad.detail.empty());
    HttpResponse wrong_name = trusted->perform(get(kHttpsIp + "/ok"), {});  // cert is for "localhost"
    CHECK(wrong_name.error == NetError::Tls);
}

TEST("connection refused and invalid schemes") {
    NEED_SERVER();
    auto t = transport();
    REQUIRE(t);
    CHECK(t->perform(get("http://127.0.0.1:9/"), {}).error == NetError::Connect);  // discard port, closed
    CHECK(t->perform(get("ftp://127.0.0.1/"), {}).error == NetError::InvalidRequest);
}

// ---- streaming ---------------------------------------------------------------------------------

namespace {

bool is_pattern(const std::vector<uint8_t> &d, int64_t from) {
    for (size_t i = 0; i < d.size(); ++i) {
        if (d[i] != static_cast<uint8_t>((from + static_cast<int64_t>(i)) % 251)) return false;
    }
    return true;
}

std::vector<uint8_t> read_to_end(HttpStream &s) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> buf(32768);
    for (;;) {
        const int64_t n = s.read(buf.data(), buf.size());
        if (n <= 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + n);
    }
    return out;
}

}  // namespace

TEST("stream: body arrives in chunks without the response size limit; no content encoding") {
    NEED_SERVER();
    auto t = transport();
    REQUIRE(t);
    HttpRequest req = get(kHttp + "/range?n=3000000");
    req.max_response_bytes = 1000;  // does not apply to streams
    long status = 0;
    size_t bytes = 0, chunks = 0;
    std::string range_header;
    HttpStreamSink sink;
    sink.on_response = [&](long st, const Headers &h) {
        status = st;
        for (const auto &[k, v] : h) {
            if (k == "accept-ranges") range_header = v;
        }
        return true;
    };
    sink.on_data = [&](const uint8_t *, size_t n) {
        bytes += n;
        ++chunks;
        return true;
    };
    HttpResponse r = t->stream(req, {}, sink);
    CHECK(r.error == NetError::None);
    CHECK_EQ(status, 200);
    CHECK_EQ(range_header, "bytes");
    CHECK_EQ(bytes, 3000000u);
    CHECK(chunks > 1);
    CHECK(r.body.empty());

    // The receiver can stop a transfer; that reads as Cancelled.
    sink.on_data = [&](const uint8_t *, size_t) { return false; };
    r = t->stream(req, {}, sink);
    CHECK(r.error == NetError::Cancelled);
    CHECK_EQ(r.status, 200);

    // Empty bodies still deliver the response.
    status = 0;
    r = t->stream(get(kHttp + "/status/204"), {}, sink);
    CHECK(r.error == NetError::None);
    CHECK_EQ(status, 204);
}

TEST("stream: HttpStream over curl reads, seeks with range requests and resumes after drops") {
    NEED_SERVER();
    std::shared_ptr<HttpTransport> t = transport();
    REQUIRE(t);
    HttpStreamOptions o;
    o.buffer_bytes = 256u << 10;
    o.retry_backoff_ms = 10;
    HttpStream s(t, kHttp + "/range?n=2000000", o);
    REQUIRE(s.open());
    CHECK(s.seekable());
    CHECK_EQ(s.size(), 2000000);
    std::vector<uint8_t> head(1000);
    REQUIRE(s.read(head.data(), head.size()) > 0);
    CHECK(is_pattern({head.begin(), head.begin() + 100}, 0));
    REQUIRE(s.seek(1900000));
    auto tail = read_to_end(s);
    CHECK_EQ(tail.size(), 100000u);
    CHECK(is_pattern(tail, 1900000));
    CHECK_EQ(s.stats().requests, 2);

    // Every connection is dropped after 300 kB: the stream reconnects at the right offsets.
    HttpStream d(t, kHttp + "/range?n=1000000&drop=300000", o);
    REQUIRE(d.open());
    auto all = read_to_end(d);
    CHECK_EQ(all.size(), 1000000u);
    CHECK(is_pattern(all, 0));
    CHECK(d.stats().reconnects >= 3);

    // A server without ranges: forward only.
    HttpStream n(t, kHttp + "/range?n=3000000&noranges=1", o);
    REQUIRE(n.open());
    CHECK(!n.seekable());
    CHECK_EQ(n.size(), 3000000);
    CHECK(!n.seek(2900000));
    auto whole = read_to_end(n);
    CHECK_EQ(whole.size(), 3000000u);
    CHECK(is_pattern(whole, 0));

    HttpStream missing(t, kHttp + "/status/404", o);
    CHECK(!missing.open());
    CHECK_EQ(missing.http_status(), 404);
}

TEST("stream: HTTPS with the kit's TLS rules") {
    NEED_SERVER();
    std::shared_ptr<HttpTransport> t = transport(true);
    REQUIRE(t);
    HttpStream s(t, kHttps + "/range?n=300000");
    REQUIRE(s.open());
    REQUIRE(s.seek(250000));
    auto tail = read_to_end(s);
    CHECK_EQ(tail.size(), 50000u);
    CHECK(is_pattern(tail, 250000));
    HttpStream bad(std::shared_ptr<HttpTransport>(transport(false)), kHttps + "/range?n=10");
    CHECK(!bad.open());
    CHECK(bad.error() == NetError::Tls);
}
