// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/net/websocket.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace wnk::net::ws {
namespace {

constexpr const char *kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool has_token(const std::string &value, const std::string &token) {
    size_t start = 0;
    const std::string v = lower(value);
    while (start <= v.size()) {
        const size_t comma = v.find(',', start);
        if (trim(v.substr(start, comma == std::string::npos ? std::string::npos : comma - start)) == token) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

}  // namespace

std::string sha1(std::string_view data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string msg(data);
    const uint64_t bits = static_cast<uint64_t>(msg.size()) * 8;
    msg += static_cast<char>(0x80);
    while (msg.size() % 64 != 56) msg += '\0';
    for (int i = 7; i >= 0; --i) msg += static_cast<char>((bits >> (i * 8)) & 0xFF);
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto *p = reinterpret_cast<const unsigned char *>(msg.data() + chunk + i * 4);
            w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::string out(20, '\0');
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<char>((h[i] >> (24 - j * 8)) & 0xFF);
    }
    return out;
}

std::string base64(std::string_view in) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += a[v >> 18];
        out += a[(v >> 12) & 63];
        out += a[(v >> 6) & 63];
        out += a[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint8_t(in[i]) << 16;
        if (i + 1 < in.size()) v |= uint8_t(in[i + 1]) << 8;
        out += a[v >> 18];
        out += a[(v >> 12) & 63];
        out += i + 1 < in.size() ? a[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string make_key(const uint8_t random16[16]) { return base64(std::string_view(reinterpret_cast<const char *>(random16), 16)); }

std::string accept_for(const std::string &key) { return base64(sha1(key + kGuid)); }

bool valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t n;
        uint32_t cp;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + n >= s.size()) return false;  // truncated sequence
        for (size_t k = 1; k <= n; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Overlong forms, surrogates and values past U+10FFFF are invalid.
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += n + 1;
    }
    return true;
}

std::string encode_frame(Opcode op, std::string_view payload, bool fin, const uint8_t mask[4]) {
    std::string f;
    f.reserve(payload.size() + 14);
    f += static_cast<char>((fin ? 0x80 : 0) | static_cast<uint8_t>(op));
    const size_t n = payload.size();
    if (n < 126) {
        f += static_cast<char>(0x80 | n);
    } else if (n <= 0xFFFF) {
        f += static_cast<char>(0x80 | 126);
        f += static_cast<char>(n >> 8);
        f += static_cast<char>(n & 0xFF);
    } else {
        f += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; --i) f += static_cast<char>((static_cast<uint64_t>(n) >> (i * 8)) & 0xFF);
    }
    f.append(reinterpret_cast<const char *>(mask), 4);
    for (size_t i = 0; i < n; ++i) f += static_cast<char>(payload[i] ^ mask[i % 4]);
    return f;
}

std::string close_payload(int code, std::string_view reason) {
    std::string p;
    p += static_cast<char>((code >> 8) & 0xFF);
    p += static_cast<char>(code & 0xFF);
    // Control payloads are at most 125 bytes; cut the reason on a UTF-8 boundary.
    size_t len = std::min<size_t>(reason.size(), 123);
    while (len > 0 && len < reason.size() && (static_cast<unsigned char>(reason[len]) & 0xC0) == 0x80) --len;
    p.append(reason.substr(0, len));
    return p;
}

void Decoder::feed(const char *data, size_t n) {
    if (failed_) return;
    if (pos_ > 0 && pos_ == buf_.size()) {
        buf_.clear();
        pos_ = 0;
    }
    buf_.append(data, n);
}

void Decoder::fail(int code, std::string why, Event *out) {
    failed_ = true;
    out->kind = Event::Kind::Error;
    out->code = code;
    out->data = std::move(why);
}

bool Decoder::next(Event *out) {
    if (failed_) return false;
    while (true) {
        const size_t before = pos_;
        Event e;
        const bool produced = parse_one(&e);
        if (produced) {
            *out = std::move(e);
            if (pos_ > 4096 && pos_ * 2 > buf_.size()) {  // compact occasionally
                buf_.erase(0, pos_);
                pos_ = 0;
            }
            return true;
        }
        if (failed_) return false;
        if (pos_ == before) return false;  // need more bytes
    }
}

// Returns true with an event, or false: either more bytes are needed (pos_ unchanged) or a data
// fragment was consumed without completing a message (pos_ advanced).
bool Decoder::parse_one(Event *out) {
    const size_t avail = buf_.size() - pos_;
    if (avail < 2) return false;
    const auto *p = reinterpret_cast<const unsigned char *>(buf_.data() + pos_);
    const bool fin = (p[0] & 0x80) != 0;
    const uint8_t rsv = p[0] & 0x70;
    const uint8_t op = p[0] & 0x0F;
    const bool masked = (p[1] & 0x80) != 0;
    uint64_t len = p[1] & 0x7F;
    size_t header = 2;
    if (len == 126) {
        if (avail < 4) return false;
        len = (uint64_t(p[2]) << 8) | p[3];
        header = 4;
    } else if (len == 127) {
        if (avail < 10) return false;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
        header = 10;
    }
    if (rsv != 0) {
        fail(kProtocolError, "reserved bits set (no extension was negotiated)", out);
        return true;
    }
    if (masked) {
        fail(kProtocolError, "server frames must not be masked", out);
        return true;
    }
    const bool control = (op & 0x8) != 0;
    if (op != 0x0 && op != 0x1 && op != 0x2 && op != 0x8 && op != 0x9 && op != 0xA) {
        fail(kProtocolError, "unknown opcode " + std::to_string(op), out);
        return true;
    }
    if (control && (!fin || len > 125)) {
        fail(kProtocolError, "control frames must be short and unfragmented", out);
        return true;
    }
    if (!control && (len > max_ || message_.size() + len > max_)) {
        fail(kTooBig, "message larger than " + std::to_string(max_) + " bytes", out);
        return true;
    }
    if (avail < header + len) return false;
    const std::string payload = buf_.substr(pos_ + header, static_cast<size_t>(len));
    pos_ += header + static_cast<size_t>(len);

    if (control) {
        if (op == 0x9) {
            out->kind = Event::Kind::Ping;
            out->data = payload;
        } else if (op == 0xA) {
            out->kind = Event::Kind::Pong;
            out->data = payload;
        } else {
            out->kind = Event::Kind::Close;
            if (payload.size() == 1) {
                fail(kProtocolError, "close frame with a 1-byte payload", out);
                return true;
            }
            out->code = payload.size() >= 2 ? (uint8_t(payload[0]) << 8) | uint8_t(payload[1]) : kNoStatus;
            out->data = payload.size() > 2 ? payload.substr(2) : "";
            if (!valid_utf8(out->data)) {
                fail(kInvalidData, "close reason is not UTF-8", out);
                return true;
            }
        }
        return true;
    }
    if (op == 0x0) {
        if (!in_message_) {
            fail(kProtocolError, "continuation without a message", out);
            return true;
        }
    } else {
        if (in_message_) {
            fail(kProtocolError, "new message before the previous one finished", out);
            return true;
        }
        in_message_ = true;
        message_op_ = static_cast<Opcode>(op);
        message_.clear();
    }
    message_ += payload;
    if (!fin) return false;  // consumed a fragment; caller loops
    in_message_ = false;
    if (message_op_ == Opcode::Text && !valid_utf8(message_)) {
        fail(kInvalidData, "text message is not UTF-8", out);
        return true;
    }
    out->kind = message_op_ == Opcode::Text ? Event::Kind::Text : Event::Kind::Binary;
    out->data = std::move(message_);
    message_.clear();
    return true;
}

bool check_handshake_response(const std::string &head, const std::string &key, const std::string &protocol_wanted,
                              std::string *protocol_out, std::string *why) {
    const size_t eol = head.find("\r\n");
    const std::string status = head.substr(0, eol);
    if (status.rfind("HTTP/1.1 101", 0) != 0) {
        *why = "server answered \"" + status + "\" instead of 101 Switching Protocols";
        return false;
    }
    std::string upgrade, connection, accept, protocol;
    size_t pos = eol == std::string::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        const size_t e = head.find("\r\n", pos);
        const std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? head.size() : e + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (name == "upgrade") upgrade = value;
        if (name == "connection") connection = value;
        if (name == "sec-websocket-accept") accept = value;
        if (name == "sec-websocket-protocol") protocol = value;
        if (name == "sec-websocket-extensions" && !value.empty()) {
            *why = "server enabled an extension that was not offered: " + value;
            return false;
        }
    }
    if (lower(upgrade) != "websocket") {
        *why = "missing Upgrade: websocket";
        return false;
    }
    if (!has_token(connection, "upgrade")) {
        *why = "missing Connection: upgrade";
        return false;
    }
    if (accept != accept_for(key)) {
        *why = "Sec-WebSocket-Accept does not match the key";
        return false;
    }
    if (!protocol.empty() && protocol_wanted.find(protocol) == std::string::npos) {
        *why = "server chose a subprotocol that was not offered: " + protocol;
        return false;
    }
    if (protocol_out != nullptr) *protocol_out = protocol;
    return true;
}

}  // namespace wnk::net::ws
