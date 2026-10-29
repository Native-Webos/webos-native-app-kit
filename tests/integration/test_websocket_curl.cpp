// SPDX-License-Identifier: GPL-3.0-or-later
//
// WebSocketClient against the local server of http_server.py (ws:// and wss:// with a throwaway CA),
// through the system libcurl. Run by CTest through http_server.py, which sets WNK_TEST_HTTP_BASE,
// WNK_TEST_HTTPS_BASE and WNK_TEST_CA.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/net/websocket.hpp"

#include <chrono>
#include <cstdlib>
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

std::string ws_base() {
    std::string b = env("WNK_TEST_HTTP_BASE");  // http://127.0.0.1:port
    return b.empty() ? "" : "ws" + b.substr(4);
}
std::string wss_base() {
    std::string b = env("WNK_TEST_HTTPS_BASE");  // https://localhost:port
    return b.empty() ? "" : "wss" + b.substr(5);
}

struct Rig {
    async::MainDispatcher main;
    std::unique_ptr<WebSocketClient> ws;
    bool opened = false;
    std::string protocol;
    std::vector<std::pair<std::string, bool>> messages;
    std::vector<std::string> errors;
    int close_code = 0;
    std::string close_reason;
    bool closed = false, clean = false;
    int reconnects = 0;

    explicit Rig(WebSocketOptions o = {}) {
        ws = std::make_unique<WebSocketClient>(main, o);
        ws->on_open = [this](const std::string &p) {
            opened = true;
            protocol = p;
        };
        ws->on_message = [this](std::string d, bool b) { messages.emplace_back(std::move(d), b); };
        ws->on_error = [this](const std::string &e) { errors.push_back(e); };
        ws->on_close = [this](int c, const std::string &r, bool cl) {
            closed = true;
            close_code = c;
            close_reason = r;
            clean = cl;
        };
        ws->on_reconnecting = [this](int, int) { ++reconnects; };
    }
    template <typename P>
    bool wait(P pred, std::chrono::milliseconds timeout = 5000ms) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < end) {
            main.drain();
            if (pred()) return true;
            std::this_thread::sleep_for(2ms);
        }
        main.drain();
        return pred();
    }
    bool wait_messages(size_t n) {
        return wait([&] { return messages.size() >= n; });
    }
};

#define NEED_SERVER()                                                   \
    if (ws_base().empty()) {                                            \
        std::printf("  SKIP: run through http_server.py\n");            \
        return;                                                         \
    }

}  // namespace

TEST("websocket: echo text and binary, subprotocol and request headers") {
    NEED_SERVER();
    WebSocketOptions o;
    o.protocols = {"chat"};
    o.headers = {{"X-Test", "hello-header"}};
    Rig r(o);
    std::string err;
    REQUIRE(r.ws->connect(ws_base() + "/ws/echo", &err));
    REQUIRE(r.wait([&] { return r.opened; }));
    CHECK_EQ(r.protocol, std::string("chat"));
    CHECK(r.ws->state() == WsState::Open);
    CHECK(r.ws->send_text("héllo"));
    CHECK(r.ws->send_binary(std::string("\0\1\2\3", 4)));
    CHECK(r.ws->send_text("header"));
    REQUIRE(r.wait_messages(3));
    CHECK(r.messages[0].first == "héllo" && !r.messages[0].second);
    CHECK(r.messages[1].first == std::string("\0\1\2\3", 4) && r.messages[1].second);
    CHECK_EQ(r.messages[2].first, std::string("hello-header"));
    CHECK(!r.ws->send_text("\xff"));  // not UTF-8: refused locally
}

TEST("websocket: wss with the right CA works; without it the certificate is refused") {
    NEED_SERVER();
    WebSocketOptions o;
    o.ca_bundle = env("WNK_TEST_CA");
    Rig r(o);
    REQUIRE(r.ws->connect(wss_base() + "/ws/echo"));
    REQUIRE(r.wait([&] { return r.opened; }));
    r.ws->send_text("secure");
    REQUIRE(r.wait_messages(1));
    CHECK_EQ(r.messages[0].first, std::string("secure"));

    Rig bad;  // system CAs only: the test CA is unknown
    REQUIRE(bad.ws->connect(wss_base() + "/ws/echo"));
    REQUIRE(bad.wait([&] { return !bad.errors.empty(); }));
    CHECK(!bad.opened);
    CHECK(bad.errors[0].find("connect failed") != std::string::npos);
}

TEST("websocket: server ping is answered, fragments reassembled, large messages delivered") {
    NEED_SERVER();
    Rig r;
    REQUIRE(r.ws->connect(ws_base() + "/ws/echo"));
    REQUIRE(r.wait([&] { return r.opened; }));
    r.ws->send_text("ping-me");
    r.ws->send_text("fragment-me");
    r.ws->send_text("big");
    r.ws->send_text("after");
    REQUIRE(r.wait_messages(3));
    // The server echoes our pong as a pong (ignored); then the fragmented text, the binary, "after".
    CHECK_EQ(r.messages[0].first, std::string("fragmented"));
    CHECK_EQ(r.messages[1].first.size(), size_t{200000});
    CHECK(r.messages[1].second);
    CHECK_EQ(r.messages[2].first, std::string("after"));
}

TEST("websocket: server-initiated and client-initiated close are clean") {
    NEED_SERVER();
    Rig a;
    REQUIRE(a.ws->connect(ws_base() + "/ws/echo"));
    REQUIRE(a.wait([&] { return a.opened; }));
    a.ws->send_text("close-me");
    REQUIRE(a.wait([&] { return a.closed; }));
    CHECK(a.clean);
    CHECK_EQ(a.close_code, 1000);
    CHECK_EQ(a.close_reason, std::string("bye"));

    Rig b;
    REQUIRE(b.ws->connect(ws_base() + "/ws/echo"));
    REQUIRE(b.wait([&] { return b.opened; }));
    b.ws->close(4001, "done");
    REQUIRE(b.wait([&] { return b.closed; }));
    CHECK(b.clean);
    CHECK_EQ(b.close_code, 4001);  // the server echoes our code
    CHECK(!b.ws->send_text("late"));
    REQUIRE(b.wait([&] { return b.ws->state() == WsState::Closed; }));
}

TEST("websocket: a protocol violation closes with 1002; a refused handshake reports the status") {
    NEED_SERVER();
    Rig r;
    REQUIRE(r.ws->connect(ws_base() + "/ws/echo"));
    REQUIRE(r.wait([&] { return r.opened; }));
    r.ws->send_text("bad-frame");
    REQUIRE(r.wait([&] { return r.closed; }));
    CHECK_EQ(r.close_code, 1002);
    REQUIRE(!r.errors.empty());
    CHECK(r.errors[0].find("masked") != std::string::npos);

    Rig rej;
    REQUIRE(rej.ws->connect(ws_base() + "/ws/reject"));
    REQUIRE(rej.wait([&] { return !rej.errors.empty(); }));
    CHECK(!rej.opened);
    CHECK(rej.errors[0].find("403") != std::string::npos);
    CHECK(!rej.ws->connect("http://example.com/"));  // not a WebSocket URL
}

TEST("websocket: dropped connections are retried with backoff up to the limit, even if each one opened") {
    NEED_SERVER();
    WebSocketOptions o;
    o.reconnect.enabled = true;
    o.reconnect.initial_ms = 50;
    o.reconnect.max_attempts = 2;
    Rig r(o);
    REQUIRE(r.ws->connect(ws_base() + "/ws/drop"));
    REQUIRE(r.wait([&] { return r.ws->state() == WsState::Closed; }));
    CHECK_EQ(r.reconnects, 2);
    CHECK(r.closed);
    CHECK(!r.clean);
    CHECK_EQ(r.close_code, 1006);
}

TEST("websocket: no pong within the timeout ends the connection; destruction is prompt") {
    NEED_SERVER();
    WebSocketOptions o;
    o.ping_interval_ms = 150;
    o.pong_timeout_ms = 200;
    Rig r(o);
    REQUIRE(r.ws->connect(ws_base() + "/ws/silent"));
    REQUIRE(r.wait([&] { return r.closed; }, 3000ms));
    REQUIRE(!r.errors.empty());
    CHECK(r.errors[0].find("no pong") != std::string::npos);

    // A long close timeout the destructor must not wait out: it caps its wait for the peer at 500 ms, so
    // a regression shows as 30 s, far from what a loaded machine adds to the 0.5 s.
    WebSocketOptions slow_close;
    slow_close.close_timeout_ms = 30000;
    Rig q(slow_close);
    REQUIRE(q.ws->connect(ws_base() + "/ws/silent"));
    REQUIRE(q.wait([&] { return q.opened; }));
    auto t0 = std::chrono::steady_clock::now();
    q.ws.reset();  // close 1001, short wait for the (absent) answer, join
    CHECK(std::chrono::steady_clock::now() - t0 < 10s);
}
