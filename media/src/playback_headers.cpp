// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/playback_headers.hpp"

#include <algorithm>
#include <cctype>

namespace wnk::media {

namespace {
bool same_name(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}
}  // namespace

void header_set(net::Headers *headers, std::string_view name, std::string_view value) {
    auto first = std::find_if(headers->begin(), headers->end(), [&](const auto &h) { return same_name(h.first, name); });
    if (first == headers->end()) {
        headers->emplace_back(std::string(name), std::string(value));
        return;
    }
    first->first = std::string(name);
    first->second = std::string(value);
    headers->erase(std::remove_if(first + 1, headers->end(), [&](const auto &h) { return same_name(h.first, name); }),
                   headers->end());
}

const std::string *header_get(const net::Headers &headers, std::string_view name) {
    for (const auto &h : headers) {
        if (same_name(h.first, name)) return &h.second;
    }
    return nullptr;
}

bool header_remove(net::Headers *headers, std::string_view name) {
    const size_t before = headers->size();
    headers->erase(std::remove_if(headers->begin(), headers->end(), [&](const auto &h) { return same_name(h.first, name); }),
                   headers->end());
    return headers->size() != before;
}

PlaybackLink parse_playback_link(std::string_view link) {
    PlaybackLink out;
    size_t bar = link.find('|');
    out.url = std::string(link.substr(0, bar));
    while (bar != std::string_view::npos) {
        const size_t start = bar + 1;
        bar = link.find('|', start);
        const std::string_view part = link.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start);
        const size_t eq = part.find('=');
        if (eq == 0 || part.empty()) continue;
        header_set(&out.headers, part.substr(0, eq), eq == std::string_view::npos ? std::string_view() : part.substr(eq + 1));
    }
    return out;
}

std::string decode_path_slashes(std::string_view url) {
    const size_t end = std::min(url.find_first_of("?#"), url.size());
    std::string out;
    out.reserve(url.size());
    for (size_t i = 0; i < url.size(); ++i) {
        if (i + 2 < end && url[i] == '%' && url[i + 1] == '2' && (url[i + 2] == 'F' || url[i + 2] == 'f')) {
            out.push_back('/');
            i += 2;
        } else {
            out.push_back(url[i]);
        }
    }
    return out;
}

void normalize_playback_headers(net::Headers *headers, std::string_view default_user_agent) {
    if (!default_user_agent.empty() && header_get(*headers, "User-Agent") == nullptr) {
        headers->emplace_back("User-Agent", std::string(default_user_agent));
    }
    if (header_get(*headers, "Accept") == nullptr) headers->emplace_back("Accept", "*/*");
}

}  // namespace wnk::media
