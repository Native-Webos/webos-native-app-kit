// SPDX-License-Identifier: GPL-3.0-or-later
// WebSocket codec: framing, decoding, protocol errors, handshake (RFC 6455 test vectors).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/net/websocket.hpp"

#include <string>

using namespace wnk::net;
using namespace wnk::net::ws;

namespace {
const uint8_t kMask[4] = {0x37, 0xfa, 0x21, 0x3d};  // RFC 6455 §5.7 example mask

// A server frame (unmasked).
std::string server_frame(uint8_t first, const std::string &payload) {
    std::string f;
    f += static_cast<char>(first);
    if (payload.size() < 126) {
        f += static_cast<char>(payload.size());
    } else if (payload.size() <= 0xFFFF) {
        f += static_cast<char>(126);
        f += static_cast<char>(payload.size() >> 8);
        f += static_cast<char>(payload.size() & 0xFF);
    } else {
        f += static_cast<char>(127);
        for (int i = 7; i >= 0; --i) f += static_cast<char>((static_cast<uint64_t>(payload.size()) >> (i * 8)) & 0xFF);
    }
    return f + payload;
}

std::vector<Event> decode_all(Decoder &d) {
    std::vector<Event> out;
    Event e;
    while (d.next(&e)) out.push_back(e);
    return out;
}
}  // namespace

TEST("sha1 and accept key match RFC 3174 / RFC 6455 vectors") {
    CHECK_EQ(base64(sha1("abc")), std::string("qZk+NkcGgWq6PiVxeFDCbJzQ2J0="));
    CHECK_EQ(accept_for("dGhlIHNhbXBsZSBub25jZQ=="), std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));  // §1.3
    const uint8_t r[16] = {'t', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};
    CHECK_EQ(make_key(r), std::string("dGhlIHNhbXBsZSBub25jZQ=="));
}

TEST("client frames are masked exactly like the RFC example") {
    // §5.7: a single-frame masked text "Hello".
    const std::string f = encode_frame(Opcode::Text, "Hello", true, kMask);
    const std::string expected("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11);
    CHECK(f == expected);
    CHECK_EQ(encode_frame(Opcode::Binary, std::string(200, 'a'), true, kMask).size(), size_t{2 + 2 + 4 + 200});
    CHECK_EQ(encode_frame(Opcode::Binary, std::string(70000, 'a'), true, kMask).size(), size_t{2 + 8 + 4 + 70000});
}

TEST("decoder: text, binary, 16- and 64-bit lengths, split across feeds") {
    Decoder d(1 << 20);
    std::string stream = server_frame(0x81, "Hello") + server_frame(0x82, std::string(300, 'b')) +
                         server_frame(0x82, std::string(70000, 'c'));
    // Feed one byte at a time for the first frames, then the rest.
    for (size_t i = 0; i < 20; ++i) d.feed(stream.data() + i, 1);
    d.feed(stream.data() + 20, stream.size() - 20);
    const auto ev = decode_all(d);
    REQUIRE(ev.size() == 3);
    CHECK(ev[0].kind == Event::Kind::Text && ev[0].data == "Hello");
    CHECK(ev[1].kind == Event::Kind::Binary && ev[1].data.size() == 300);
    CHECK(ev[2].data.size() == 70000);
}

TEST("decoder: fragmented message with an interleaved ping (RFC §5.4)") {
    Decoder d;
    const std::string s = server_frame(0x01, "Hel") + server_frame(0x89, "p") + server_frame(0x80, "lo");
    d.feed(s.data(), s.size());
    const auto ev = decode_all(d);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].kind == Event::Kind::Ping && ev[0].data == "p");
    CHECK(ev[1].kind == Event::Kind::Text && ev[1].data == "Hello");
}

TEST("decoder: close frames with and without a code") {
    Decoder d;
    std::string s = server_frame(0x88, std::string("\x03\xe8", 2) + "bye") + server_frame(0x88, "");
    d.feed(s.data(), s.size());
    const auto ev = decode_all(d);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].kind == Event::Kind::Close && ev[0].code == 1000 && ev[0].data == "bye");
    CHECK(ev[1].code == kNoStatus);
    CHECK_EQ(close_payload(1001, std::string(200, 'x')).size(), size_t{125});
}

TEST("decoder: protocol violations map to the right close codes") {
    struct Case {
        std::string bytes;
        int code;
    };
    const std::string masked("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11);
    const Case cases[] = {
        {masked, kProtocolError},                                   // masked server frame
        {server_frame(0xC1, "x"), kProtocolError},                  // RSV1 without an extension
        {server_frame(0x83, "x"), kProtocolError},                  // reserved opcode
        {server_frame(0x09, "x"), kProtocolError},                  // fragmented ping
        {server_frame(0x89, std::string(126, 'x')), kProtocolError},  // long ping
        {server_frame(0x80, "x"), kProtocolError},                  // continuation without start
        {server_frame(0x01, "a") + server_frame(0x81, "b"), kProtocolError},  // new message mid-fragment
        {server_frame(0x81, "\xff\xfe"), kInvalidData},             // not UTF-8
        {server_frame(0x88, "\x03"), kProtocolError},               // 1-byte close payload
    };
    for (const auto &c : cases) {
        Decoder d;
        d.feed(c.bytes.data(), c.bytes.size());
        Event e;
        REQUIRE(d.next(&e));
        CHECK(e.kind == Event::Kind::Error);
        CHECK_EQ(e.code, c.code);
        CHECK(d.failed());
    }
    Decoder small(10);
    const std::string big = server_frame(0x82, std::string(11, 'x'));
    small.feed(big.data(), big.size());
    Event e;
    REQUIRE(small.next(&e));
    CHECK_EQ(e.code, kTooBig);
}

TEST("utf-8 validation: accepts valid text, rejects overlongs, surrogates and truncation") {
    CHECK(valid_utf8("plain ascii"));
    CHECK(valid_utf8("\xce\xba\xe1\xbd\xb9\xcf\x83\xce\xbc\xce\xb5"));  // κόσμε
    CHECK(valid_utf8("\xf0\x9f\x98\x80"));                              // U+1F600
    CHECK(!valid_utf8("\xc0\xaf"));                                      // overlong '/'
    CHECK(!valid_utf8("\xed\xa0\x80"));                                  // surrogate
    CHECK(!valid_utf8("\xf4\x90\x80\x80"));                              // > U+10FFFF
    CHECK(!valid_utf8("\xe2\x82"));                                      // truncated
}

TEST("handshake response: accepted, and each failure explained") {
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string good = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Protocol: chat\r\n\r\n";
    std::string proto, why;
    CHECK(check_handshake_response(good, key, "chat, superchat", &proto, &why));
    CHECK_EQ(proto, std::string("chat"));
    CHECK(!check_handshake_response("HTTP/1.1 403 Forbidden\r\n\r\n", key, "", &proto, &why));
    CHECK(why.find("403") != std::string::npos);
    std::string wrong = good;
    wrong.replace(wrong.find("s3pP"), 4, "AAAA");
    CHECK(!check_handshake_response(wrong, key, "chat", &proto, &why));
    CHECK(why.find("Accept") != std::string::npos);
    CHECK(!check_handshake_response(good, key, "other", &proto, &why));  // protocol not offered
    const std::string ext = "HTTP/1.1 101 x\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
                            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n";
    CHECK(!check_handshake_response(ext, key, "", &proto, &why));
}
