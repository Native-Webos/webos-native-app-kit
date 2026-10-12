// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/clearkey.hpp"

#include "wnk/log.hpp"

#include <nlohmann/json.hpp>

#include <cctype>

namespace wnk::media {

namespace {

constexpr const char *kCat = "media.drm";
using nlohmann::json;

std::string hex_of(const std::string &bytes) {
    static const char *kDigits = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : bytes) {
        out.push_back(kDigits[c >> 4]);
        out.push_back(kDigits[c & 15]);
    }
    return out;
}

// Unpadded (or padded) base64url → bytes; "" on malformed input.
std::string base64url_decode(std::string_view in) {
    while (!in.empty() && in.back() == '=') in.remove_suffix(1);
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else return {};
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xff));
        }
    }
    return out;
}

std::string base64url_encode(const std::string &bytes) {
    static const char *kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (const unsigned char c : bytes) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(kAlphabet[(acc >> bits) & 63]);
        }
    }
    if (bits > 0) out.push_back(kAlphabet[(acc << (6 - bits)) & 63]);
    return out;
}

std::string bytes_of_hex(const std::string &hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

bool keys_from_jwk(const json &doc, ContentKeys *out, std::string *error) {
    if (!doc.is_object() || !doc.contains("keys") || !doc["keys"].is_array()) {
        *error = "not a JSON Web Key Set";
        return false;
    }
    ContentKeys got;
    for (const json &k : doc["keys"]) {
        if (!k.is_object() || !k.contains("kid") || !k.contains("k") || !k["kid"].is_string() || !k["k"].is_string()) {
            *error = "a key without kid or k";
            return false;
        }
        const std::string kid = normalize_key_id(k["kid"].get<std::string>());
        const std::string key = normalize_key_id(k["k"].get<std::string>());
        if (kid.empty() || key.empty()) {
            *error = "a key that is not 16 bytes";
            return false;
        }
        got[kid] = key;
    }
    out->insert(got.begin(), got.end());
    return true;
}

}  // namespace

std::string normalize_key_id(std::string_view text) {
    std::string hex;
    for (const char c : text) {
        if (c == '-') continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            hex.clear();
            break;
        }
        hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (hex.size() == 32) return hex;
    // Not hex: base64url (JWK) of 16 bytes is 22 characters, 24 when padded.
    if (text.size() == 22 || (text.size() == 24 && text.substr(22) == "==")) {
        const std::string bytes = base64url_decode(text);
        if (bytes.size() == 16) return hex_of(bytes);
    }
    return {};
}

bool parse_content_keys(std::string_view text, ContentKeys *out, std::string *error) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first != std::string_view::npos && text[first] == '{') {
        const json doc = json::parse(text.begin(), text.end(), nullptr, false);
        if (doc.is_discarded()) {
            *error = "keys: not valid JSON";
            return false;
        }
        return keys_from_jwk(doc, out, error);
    }
    ContentKeys got;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ',' || std::isspace(static_cast<unsigned char>(text[i])))) ++i;
        if (i >= text.size()) break;
        size_t end = i;
        while (end < text.size() && text[end] != ',' && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        const std::string_view pair = text.substr(i, end - i);
        size_t sep = pair.find(':');
        if (sep == std::string_view::npos) sep = pair.find('=');
        const std::string kid = sep == std::string_view::npos ? std::string() : normalize_key_id(pair.substr(0, sep));
        const std::string key = sep == std::string_view::npos ? std::string() : normalize_key_id(pair.substr(sep + 1));
        if (kid.empty() || key.empty()) {
            *error = "keys: expected kid:key with 32 hex digits each (entry " + std::to_string(got.size() + 1) + ")";
            return false;
        }
        got[kid] = key;
        i = end;
    }
    if (got.empty()) {
        *error = "keys: none given";
        return false;
    }
    out->insert(got.begin(), got.end());
    return true;
}

std::string clearkey_license_request(const std::vector<std::string> &key_ids) {
    json kids = json::array();
    for (const std::string &k : key_ids) {
        const std::string hex = normalize_key_id(k);
        if (!hex.empty()) kids.push_back(base64url_encode(bytes_of_hex(hex)));
    }
    return json{{"kids", kids}, {"type", "temporary"}}.dump();
}

bool parse_clearkey_license(std::string_view body, ContentKeys *out, std::string *error) {
    const json doc = json::parse(body.begin(), body.end(), nullptr, false);
    if (doc.is_discarded()) {
        *error = "licence: not valid JSON";
        return false;
    }
    if (!keys_from_jwk(doc, out, error)) {
        *error = "licence: " + *error;
        return false;
    }
    return true;
}

bool fetch_clearkey_license(net::HttpTransport &http, const std::string &url, const net::Headers &headers,
                            const std::vector<std::string> &key_ids, const async::CancellationToken &token,
                            ContentKeys *out, std::string *error) {
    net::HttpRequest req;
    req.method = "POST";
    req.url = url;
    req.headers = headers;
    req.headers.emplace_back("Content-Type", "application/json");
    req.body = clearkey_license_request(key_ids);
    req.timeout_ms = 10000;
    req.max_response_bytes = 256u << 10;
    req.idempotent = true;  // asking for keys changes nothing on the server
    const net::HttpResponse r = http.perform(req, token);
    if (r.error != net::NetError::None) {
        *error = std::string("licence request failed: ") + net::error_name(r.error);
        return false;
    }
    if (r.status < 200 || r.status >= 300) {
        *error = "licence server answered HTTP " + std::to_string(r.status);
        return false;
    }
    ContentKeys got;
    if (!parse_clearkey_license(r.body, &got, error)) return false;
    size_t wanted = 0;
    for (const std::string &k : key_ids) wanted += got.count(normalize_key_id(k));
    if (wanted == 0) {
        *error = "the licence has none of the " + std::to_string(key_ids.size()) + " requested key(s)";
        return false;
    }
    WNK_LOGI(kCat, "licence: %zu key(s) for %zu requested key ID(s)", got.size(), key_ids.size());
    out->insert(got.begin(), got.end());
    return true;
}

std::string ffmpeg_decryption_keys(const ContentKeys &keys) {
    std::string out;
    for (const auto &[kid, key] : keys) {
        if (!out.empty()) out += ':';
        out += kid + "=" + key;
    }
    return out;
}

}  // namespace wnk::media
