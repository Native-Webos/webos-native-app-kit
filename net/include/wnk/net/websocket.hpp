// SPDX-License-Identifier: GPL-3.0-or-later
//
// WebSocket client (RFC 6455) for native apps.
//
// The TV's libcurl is too old for curl's own WebSocket API (7.86+; webOS 6.5.3 has 7.61), so the
// connection is opened with CURLOPT_CONNECT_ONLY — curl still does DNS, TCP, TLS with certificate
// verification, and proxies — and the handshake and framing run on top through curl_easy_send /
// curl_easy_recv (7.18+), polling the socket from CURLINFO_ACTIVESOCKET (7.45+).
//
//   ws::  frame codec, handshake helpers and UTF-8 validation (pure, host-tested)
//   WebSocketClient  one I/O thread per connection; sends from any thread; callbacks on the main thread
//
// Not supported: extensions (permessage-deflate), server mode.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/net/http.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wnk::net {

namespace ws {

enum class Opcode : uint8_t { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

// Close codes the client sends or reports (RFC 6455 §7.4.1).
inline constexpr int kNormal = 1000;
inline constexpr int kGoingAway = 1001;
inline constexpr int kProtocolError = 1002;
inline constexpr int kNoStatus = 1005;    // reported, never sent: close frame without a code
inline constexpr int kAbnormal = 1006;    // reported, never sent: connection lost without a close frame
inline constexpr int kInvalidData = 1007;  // text that is not UTF-8
inline constexpr int kTooBig = 1009;

// One frame as a client sends it: always masked with `mask`.
std::string encode_frame(Opcode op, std::string_view payload, bool fin, const uint8_t mask[4]);
// Close frame payload: 2-byte code + UTF-8 reason (truncated to fit a control frame).
std::string close_payload(int code, std::string_view reason);

struct Event {
    enum class Kind : uint8_t { Text, Binary, Ping, Pong, Close, Error } kind = Kind::Text;
    std::string data;  // message, ping/pong payload, close reason, or error text
    int code = 0;      // Close: the peer's code (kNoStatus if none); Error: the close code to send
};

// Incremental decoder for frames from a server: reassembles fragmented messages, validates the
// protocol (unmasked frames, no reserved bits, known opcodes, short unfragmented control frames,
// UTF-8 text, size limit). After an Error event the decoder stops.
class Decoder {
 public:
    explicit Decoder(size_t max_message_bytes = 1u << 20) : max_(max_message_bytes) {}
    void feed(const char *data, size_t n);
    // Next complete event, if any.
    bool next(Event *out);
    bool failed() const { return failed_; }

 private:
    bool parse_one(Event *out);
    void fail(int code, std::string why, Event *out);
    std::string buf_;
    size_t pos_ = 0;
    size_t max_;
    bool failed_ = false;
    bool in_message_ = false;
    Opcode message_op_ = Opcode::Text;
    std::string message_;
};

bool valid_utf8(std::string_view s);
// Sec-WebSocket-Key from 16 random bytes, and the Sec-WebSocket-Accept a server must answer with.
std::string make_key(const uint8_t random16[16]);
std::string accept_for(const std::string &key);
std::string sha1(std::string_view data);  // 20 raw bytes
std::string base64(std::string_view bytes);

// Checks a server's handshake response head (status line + headers, up to the blank line).
bool check_handshake_response(const std::string &head, const std::string &key, const std::string &protocol_wanted,
                              std::string *protocol_out, std::string *why);

}  // namespace ws

struct WebSocketOptions {
    Headers headers;                     // extra request headers (e.g. Authorization)
    std::vector<std::string> protocols;  // Sec-WebSocket-Protocol offers
    std::string ca_bundle;               // PEM file instead of the system store (tests)
    int connect_timeout_ms = 10000;
    int ping_interval_ms = 30000;        // keepalive ping when idle; 0 disables
    int pong_timeout_ms = 10000;         // no pong in time: the connection is considered lost
    int close_timeout_ms = 3000;         // wait for the peer's close frame after sending ours
    size_t max_message_bytes = 1u << 20;
    struct Reconnect {
        bool enabled = false;
        int initial_ms = 1000;
        int max_ms = 30000;
        double multiplier = 2.0;
        int max_attempts = 0;  // consecutive attempts; 0 = unlimited. Backoff resets only after a
                               // connection stayed open 5 s or delivered a message.
    } reconnect;
};

enum class WsState : uint8_t { Idle, Connecting, Open, Closing, Closed };
const char *ws_state_name(WsState s);

class WebSocketClient {
 public:
    // Callbacks run on the main thread (through `main`), never after the client is destroyed.
    std::function<void(const std::string &protocol)> on_open;
    std::function<void(std::string data, bool binary)> on_message;
    // clean: a close frame was exchanged; code/reason from the peer (kAbnormal if the link dropped).
    std::function<void(int code, const std::string &reason, bool clean)> on_close;
    std::function<void(const std::string &error)> on_error;
    std::function<void(int attempt, int delay_ms)> on_reconnecting;

    WebSocketClient(async::MainDispatcher &main, WebSocketOptions options = {});
    ~WebSocketClient();  // closes (going away) and joins the I/O thread
    WebSocketClient(const WebSocketClient &) = delete;
    WebSocketClient &operator=(const WebSocketClient &) = delete;

    // ws:// or wss://. Starts the I/O thread; returns false if the URL is not a WebSocket URL or the
    // client is already running.
    bool connect(const std::string &url, std::string *error = nullptr);
    // Any thread. Queued while connecting; false when closed or closing.
    bool send_text(std::string text);
    bool send_binary(std::string bytes);
    // Starts the close handshake (no reconnect afterwards).
    void close(int code = ws::kNormal, std::string reason = {});
    WsState state() const { return state_.load(); }

    struct Impl;

 private:
    bool enqueue(ws::Opcode op, std::string payload);
    void run(std::string url);
    void post(std::function<void()> fn);

    async::MainDispatcher &main_;
    WebSocketOptions options_;
    std::shared_ptr<int> alive_;
    std::atomic<WsState> state_{WsState::Idle};
    std::thread thread_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wnk::net
