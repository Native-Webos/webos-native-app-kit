// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/thumbnails.hpp"

#include "wnk/media/manifest.hpp"
#include "wnk/media/webvtt.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace wnk::media {

namespace {

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

}  // namespace

bool parse_tile_layout(const std::string &text, int *columns, int *rows) {
    const std::string t = trim(text);
    const size_t x = t.find_first_of("xX");
    if (x == std::string::npos || x == 0 || x + 1 >= t.size()) return false;
    const std::string a = t.substr(0, x), b = t.substr(x + 1);
    if (a.find_first_not_of("0123456789") != std::string::npos || b.find_first_not_of("0123456789") != std::string::npos) return false;
    const long c = std::atol(a.c_str()), r = std::atol(b.c_str());
    if (c < 1 || r < 1 || c > 1000 || r > 1000) return false;
    *columns = static_cast<int>(c);
    *rows = static_cast<int>(r);
    return true;
}

std::optional<Thumbnail> ThumbnailTrack::at(int64_t t) const {
    if (sheets.empty() || t == kNoTime) return std::nullopt;
    // The last sheet starting at or before t.
    auto it = std::upper_bound(sheets.begin(), sheets.end(), t, [](int64_t v, const Sheet &s) { return v < s.start_us; });
    if (it == sheets.begin()) return std::nullopt;
    const Sheet &s = *std::prev(it);
    if (s.duration_us <= 0 || t >= s.start_us + s.duration_us) return std::nullopt;
    const int tiles = std::max(1, s.columns * s.rows);
    const int64_t each = s.tile_duration_us > 0 ? s.tile_duration_us : std::max<int64_t>(1, s.duration_us / tiles);
    const int i = static_cast<int>(std::min<int64_t>(tiles - 1, (t - s.start_us) / each));
    Thumbnail out;
    out.url = s.url;
    out.width = s.tile_width;
    out.height = s.tile_height;
    out.x = s.x + (s.tile_width > 0 ? (i % s.columns) * s.tile_width : 0);
    out.y = s.y + (s.tile_height > 0 ? (i / s.columns) * s.tile_height : 0);
    out.start_us = s.start_us + i * each;
    out.end_us = std::min(s.start_us + s.duration_us, out.start_us + each);
    return out;
}

bool parse_thumbnail_vtt(const std::string &text, const std::string &url, ThumbnailTrack *out, std::string *error) {
    *out = ThumbnailTrack{};
    out->id = url;
    WebVttFile f;
    if (!parse_webvtt(text, &f, error)) return false;
    for (const SubtitleCue &c : f.cues) {
        const std::string ref = trim(webvtt_plain_text(c.text));
        if (ref.empty() || c.end_us <= c.start_us) continue;
        ThumbnailTrack::Sheet s;
        s.start_us = c.start_us;
        s.duration_us = c.end_us - c.start_us;
        const size_t hash = ref.find("#xywh=");
        s.url = resolve_url(url, hash == std::string::npos ? ref : ref.substr(0, hash));
        if (hash != std::string::npos) {
            int x = 0, y = 0, w = 0, h = 0;
            if (std::sscanf(ref.c_str() + hash + 6, "%d,%d,%d,%d", &x, &y, &w, &h) != 4 || x < 0 || y < 0 || w <= 0 || h <= 0) continue;
            s.x = x;
            s.y = y;
            s.tile_width = w;
            s.tile_height = h;
            if (out->tile_width == 0) out->tile_width = w, out->tile_height = h;
        }
        out->sheets.push_back(std::move(s));
    }
    std::stable_sort(out->sheets.begin(), out->sheets.end(), [](const auto &a, const auto &b) { return a.start_us < b.start_us; });
    if (out->sheets.empty()) {
        if (error != nullptr) *error = "no thumbnail cues";
        return false;
    }
    return true;
}

int choose_thumbnail_track(const std::vector<ThumbnailTrack> &tracks, int want_width) {
    int best = -1;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].empty()) continue;
        const int w = tracks[i].tile_width;
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const int bw = tracks[static_cast<size_t>(best)].tile_width;
        const bool fits = w <= want_width, best_fits = bw <= want_width;
        if ((fits && (!best_fits || w > bw)) || (!fits && !best_fits && w < bw)) best = static_cast<int>(i);
    }
    return best;
}

}  // namespace wnk::media
