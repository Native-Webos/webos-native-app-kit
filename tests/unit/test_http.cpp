// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/net/http.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

using namespace wnk;
using namespace wnk::net;
using namespace std::chrono_literals;

namespace {

// Replays scripted responses and records the requests it saw.
struct ScriptedTransport final : HttpTransport {
    std::mutex m;
    std::deque<HttpResponse> script;
    std::vector<std::string> methods;
    std::atomic<int> calls{0};
    std::chrono::milliseconds delay{0};

    HttpResponse perform(const HttpRequest &req, const async::CancellationToken &token) override {
        ++calls;
        {
            std::lock_guard<std::mutex> lock(m);
            methods.push_back(req.method);
        }
        const auto end = async::Clock::now() + delay;
        while (async::Clock::now() < end) {
            if (token.cancelled()) {
                HttpResponse r;
                r.error = NetError::Cancelled;
                return r;
            }
            std::this_thread::sleep_for(2ms);
        }
        std::lock_guard<std::mutex> lock(m);
        if (script.empty()) {
            HttpResponse r;
            r.status = 200;
            return r;
        }
        HttpResponse r = script.front();
        script.pop_front();
        return r;
    }
    std::string describe() const override { return "scripted"; }
};

HttpResponse status(long s) {
    HttpResponse r;
    r.status = s;
    return r;
}
HttpResponse failure(NetError e) {
    HttpResponse r;
    r.error = e;
    return r;
}

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit = 5000ms) {
    const auto end = async::Clock::now() + limit;
    while (!pred()) {
        if (async::Clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

RetryPolicy fast_retry() {
    RetryPolicy p;
    p.initial_backoff_ms = 5;
    p.max_backoff_ms = 20;
    return p;
}

}  // namespace

TEST("request validation") {
    HttpRequest r;
    std::string why;
    r.url = "https://example.org/path?q=1";
    CHECK(validate_request(r, &why));
    r.url = "ftp://example.org/";
    CHECK(!validate_request(r, &why));
    r.url = "https:///nohost";
    CHECK(!validate_request(r, &why));
    r.url = "http://example.org/a b";
    CHECK(!validate_request(r, &why));
    r.url = "http://example.org/";
    r.method = "get";
    CHECK(!validate_request(r, &why));
    r.method = "PATCH";
    CHECK(validate_request(r, &why));
    r.headers = {{"X-Ok", "v"}, {"Bad Name", "v"}};
    CHECK(!validate_request(r, &why));
    r.headers = {{"X-Inject", "a\r\nEvil: 1"}};
    CHECK(!validate_request(r, &why));
    CHECK(why.find("CR/LF") != std::string::npos);
}

TEST("idempotent methods") {
    CHECK(is_idempotent_method("GET"));
    CHECK(is_idempotent_method("PUT"));
    CHECK(!is_idempotent_method("POST"));
    CHECK(!is_idempotent_method("PATCH"));
}

TEST("retry policy retries transient failures only") {
    RetryPolicy p;
    CHECK(p.should_retry(failure(NetError::Connect), 1));
    CHECK(p.should_retry(failure(NetError::Timeout), 2));
    CHECK(!p.should_retry(failure(NetError::Timeout), 3));  // max_attempts reached
    CHECK(p.should_retry(failure(NetError::Dns), 1));
    CHECK(!p.should_retry(failure(NetError::Dns), 2));
    CHECK(!p.should_retry(failure(NetError::Tls), 1));
    CHECK(!p.should_retry(failure(NetError::Cancelled), 1));
    CHECK(!p.should_retry(failure(NetError::TooLarge), 1));
    CHECK(p.should_retry(status(503), 1));
    CHECK(!p.should_retry(status(500), 1));
    CHECK(!p.should_retry(status(404), 1));
    CHECK_EQ(p.backoff_ms(1), 300);
    CHECK_EQ(p.backoff_ms(2), 600);
    CHECK_EQ(p.backoff_ms(10), 3000);
}

TEST("header lookup is case-insensitive") {
    HttpResponse r;
    r.headers = {{"content-type", "application/json"}};
    CHECK_EQ(r.header("Content-Type"), std::string("application/json"));
    CHECK(r.header("x-missing").empty());
}

TEST("client retries an idempotent GET and delivers on the main thread") {
    auto t = std::make_shared<ScriptedTransport>();
    t->script = {status(503), failure(NetError::Connect), status(200)};
    async::WorkerPool pool(2);
    async::MainDispatcher main;
    HttpClient client(t, pool, main, fast_retry());
    async::Lifetime owner;
    std::optional<HttpResponse> got;
    const auto main_id = std::this_thread::get_id();
    bool on_main = false;
    HttpRequest req;
    req.url = "https://example.org/";
    REQUIRE(client.send(req, {}, owner.weak(), [&](HttpResponse r) {
        got = std::move(r);
        on_main = std::this_thread::get_id() == main_id;
    }));
    CHECK(wait_until([&] { main.drain(); return got.has_value(); }));
    REQUIRE(got.has_value());
    CHECK(got->ok());
    CHECK_EQ(got->attempts, 3);
    CHECK(on_main);
    CHECK(pool.shutdown(1000ms));
}

TEST("client never retries POST") {
    auto t = std::make_shared<ScriptedTransport>();
    t->script = {status(503)};
    async::WorkerPool pool(1);
    async::MainDispatcher main;
    HttpClient client(t, pool, main, fast_retry());
    HttpRequest req;
    req.method = "POST";
    req.url = "https://example.org/submit";
    req.body = "x=1";
    HttpResponse r = client.perform_with_retry(req, {});
    CHECK_EQ(r.status, 503L);
    CHECK_EQ(t->calls.load(), 1);
    req.idempotent = true;  // the caller may opt in explicitly
    t->script = {status(503), status(200)};
    r = client.perform_with_retry(req, {});
    CHECK_EQ(r.status, 200L);
    CHECK(pool.shutdown(1000ms));
}

TEST("cancellation stops the request and suppresses the callback") {
    auto t = std::make_shared<ScriptedTransport>();
    t->delay = 2000ms;
    async::WorkerPool pool(1);
    async::MainDispatcher main;
    HttpClient client(t, pool, main, fast_retry());
    async::Lifetime owner;
    async::CancellationSource cancel;
    bool called = false;
    HttpRequest req;
    req.url = "https://example.org/slow";
    REQUIRE(client.send(req, cancel.token(), owner.weak(), [&](HttpResponse) { called = true; }));
    CHECK(wait_until([&] { return t->calls.load() == 1; }));
    const auto t0 = async::Clock::now();
    cancel.cancel();
    CHECK(wait_until([&] { return pool.stats().completed == 1; }));
    CHECK(async::Clock::now() - t0 < 500ms);
    main.drain();
    CHECK(!called);
    CHECK(pool.shutdown(1000ms));
}

TEST("result for a destroyed owner is dropped; invalid requests are refused") {
    auto t = std::make_shared<ScriptedTransport>();
    t->delay = 50ms;
    async::WorkerPool pool(1);
    async::MainDispatcher main;
    HttpClient client(t, pool, main, fast_retry());
    bool called = false;
    {
        async::Lifetime owner;
        HttpRequest req;
        req.url = "https://example.org/";
        REQUIRE(client.send(req, {}, owner.weak(), [&](HttpResponse) { called = true; }));
    }
    CHECK(wait_until([&] { return pool.stats().completed == 1; }));
    main.drain();
    CHECK(!called);
    async::Lifetime owner;
    HttpRequest bad;
    bad.url = "file:///etc/passwd";
    CHECK(!client.send(bad, {}, owner.weak(), [&](HttpResponse) { called = true; }));
    CHECK(pool.shutdown(1000ms));
}
