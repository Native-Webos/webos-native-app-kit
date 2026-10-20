// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/iptv.hpp"

#include "wnk/media/playback_headers.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <utility>

namespace wnk::media {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

std::string_view trim(std::string_view s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// "http-referrer" → "Referer"; unknown names keep their spelling (without "http-").
std::string header_name(std::string_view name) {
    name = trim(name);
    if (starts_with(lower(name.substr(0, 5)), "http-")) name.remove_prefix(5);
    const std::string l = lower(name);
    if (l == "referer" || l == "referrer") return "Referer";
    if (l == "user-agent") return "User-Agent";
    if (l == "origin") return "Origin";
    if (l == "cookie") return "Cookie";
    if (l == "authorization") return "Authorization";
    return std::string(name);
}

// "name=value" → a header; nothing for a part without a name.
void apply_header(net::Headers *headers, std::string_view part) {
    const size_t eq = part.find('=');
    if (eq == std::string_view::npos) return;
    const std::string name = header_name(part.substr(0, eq));
    if (name.empty()) return;
    header_set(headers, name, trim(part.substr(eq + 1)));
}

// The first comma outside double quotes, or npos.
size_t title_comma(std::string_view s) {
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') quoted = !quoted;
        else if (s[i] == ',' && !quoted) return i;
    }
    return std::string_view::npos;
}

struct Pending {
    IptvChannel channel;
    std::string kid, key;
};

// "-1 tvg-id="x" group-title="News, local"": the duration, then name=value pairs, quoted or not.
void parse_extinf_attributes(std::string_view s, Pending *p) {
    size_t i = s.find_first_of(" \t");  // past the duration
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const size_t name_start = i;
        while (i < s.size() && s[i] != '=' && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const std::string name = lower(s.substr(name_start, i - name_start));
        if (i >= s.size() || s[i] != '=') continue;  // a bare word
        ++i;
        std::string_view value;
        if (i < s.size() && s[i] == '"') {
            const size_t close = s.find('"', i + 1);
            value = s.substr(i + 1, (close == std::string_view::npos ? s.size() : close) - i - 1);
            i = close == std::string_view::npos ? s.size() : close + 1;
        } else {
            const size_t v = i;
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            value = s.substr(v, i - v);
        }
        if (name == "tvg-id") p->channel.tvg_id = std::string(value);
        else if (name == "tvg-logo") p->channel.logo = std::string(value);
        else if (name == "group-title") p->channel.group = std::string(value);
        else if (name == "key" || name == "drm-key") p->key = std::string(value);
        else if (name == "keyid" || name == "drm-keyid" || name == "kid") p->kid = std::string(value);
    }
}

void parse_extinf(std::string_view s, Pending *p) {
    const size_t comma = title_comma(s);
    parse_extinf_attributes(s.substr(0, comma), p);
    if (comma != std::string_view::npos) p->channel.title = std::string(trim(s.substr(comma + 1)));
}

void parse_exthttp(std::string_view s, net::Headers *headers) {
    const auto doc = nlohmann::json::parse(s.begin(), s.end(), nullptr, false);
    if (!doc.is_object()) return;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (!it.value().is_string()) continue;
        const std::string name = header_name(it.key());
        if (!name.empty()) header_set(headers, name, it.value().get<std::string>());
    }
}

void parse_kodiprop(std::string_view s, Pending *p) {
    const size_t eq = s.find('=');
    if (eq == std::string_view::npos || lower(trim(s.substr(0, eq))) != "inputstream.adaptive.license_key") return;
    const std::string_view value = trim(s.substr(eq + 1));
    const std::string l = lower(value.substr(0, 8));
    if (starts_with(l, "http://") || starts_with(l, "https://")) {
        p->channel.license_url = std::string(value);
    } else {
        p->channel.keys_text = std::string(value);
    }
}

// The URL with its "|name=value" parameters taken off into headers.
void finish(Pending *p, std::string url, std::vector<IptvChannel> *out) {
    const size_t bar = url.find('|');
    if (bar != std::string::npos) {
        const std::string_view params = std::string_view(url).substr(bar + 1);
        size_t start = 0;
        while (start <= params.size()) {
            const size_t sep = params.find_first_of("&|", start);
            apply_header(&p->channel.headers, params.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start));
            if (sep == std::string_view::npos) break;
            start = sep + 1;
        }
        url.resize(bar);
    }
    p->channel.url = decode_path_slashes(trim(url));
    if (p->channel.keys_text.empty() && !p->kid.empty() && !p->key.empty()) p->channel.keys_text = p->kid + ":" + p->key;
    if (!p->channel.url.empty()) out->push_back(std::move(p->channel));
    *p = Pending{};
}

}  // namespace

bool parse_iptv_playlist(std::string_view text, std::vector<IptvChannel> *out, std::string *error) {
    out->clear();
    if (starts_with(text, "\xEF\xBB\xBF")) text.remove_prefix(3);
    std::vector<std::string_view> lines;
    for (size_t pos = 0; pos <= text.size();) {
        const size_t nl = text.find('\n', pos);
        const std::string_view line = trim(text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        if (!line.empty()) lines.push_back(line);
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    if (lines.empty() || !starts_with(lines[0], "#EXTM3U")) {
        *error = "not an M3U channel list (no #EXTM3U)";
        return false;
    }
    for (const auto &l : lines) {
        if (starts_with(l, "#EXT-X-")) {
            *error = "this is an HLS media playlist, not a channel list: open it with AdaptiveSource";
            return false;
        }
    }
    Pending pending;
    std::string url;  // may wrap over several lines
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string_view l = lines[i];
        if (l[0] != '#') {
            url += l;
            continue;
        }
        if (!url.empty()) finish(&pending, std::exchange(url, {}), out);
        if (starts_with(l, "#EXTINF:")) {
            parse_extinf(l.substr(8), &pending);
        } else if (starts_with(l, "#EXTVLCOPT:")) {
            apply_header(&pending.channel.headers, l.substr(11));
        } else if (starts_with(l, "#EXTHTTP:")) {
            parse_exthttp(l.substr(9), &pending.channel.headers);
        } else if (starts_with(l, "#KODIPROP:")) {
            parse_kodiprop(l.substr(10), &pending);
        }
    }
    if (!url.empty()) finish(&pending, std::move(url), out);
    if (out->empty()) {
        *error = "the channel list has no entry with a URL";
        return false;
    }
    return true;
}

}  // namespace wnk::media
