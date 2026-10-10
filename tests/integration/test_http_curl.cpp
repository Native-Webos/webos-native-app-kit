// SPDX-License-Identifier: GPL-3.0-or-later
//
// The curl transport against a local HTTP/HTTPS server (tests/integration/http_server.py starts it and
// passes its addresses through the environment). Real sockets, real TLS, nothing leaves 127.0.0.1.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/net/http.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

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
