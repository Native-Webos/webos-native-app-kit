// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/manifest.hpp"

#include "wnk/media/xml.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace wnk::media {
namespace {

constexpr size_t kMaxSegments = 1u << 20;  // per rendition: a guard against r="2000000000"

// ---- small helpers -----------------------------------------------------------------------------

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool to_i64(const std::string &s, int64_t *out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    char *end = nullptr;
    errno = 0;
    const long long v = std::strtoll(t.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0') return false;
    *out = v;
    return true;
}

int64_t i64_or(const std::string *s, int64_t fallback) {
    int64_t v = 0;
    return s != nullptr && to_i64(*s, &v) ? v : fallback;
}

double to_double(const std::string &s) {
    const std::string t = trim(s);
    char *end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    return end != nullptr && end != t.c_str() ? v : 0.0;
}

// "30000/1001" or "25" or "29.97".
double frame_rate(const std::string &s) {
    const size_t slash = s.find('/');
    if (slash == std::string::npos) return to_double(s);
    const double den = to_double(s.substr(slash + 1));
    return den > 0 ? to_double(s.substr(0, slash)) / den : 0.0;
}

// ticks / timescale in microseconds without overflow for large tick counts.
int64_t ticks_to_us(int64_t ticks, int64_t timescale) {
    if (timescale <= 0) return 0;
    const int64_t whole = ticks / timescale, rest = ticks % timescale;
    return whole * 1000000 + rest * 1000000 / timescale;
}

// "100-199" → offset 100, length 100.
bool parse_range(const std::string &s, ByteRange *out) {
    const size_t dash = s.find('-');
    if (dash == std::string::npos) return false;
    int64_t first = 0, last = 0;
    if (!to_i64(s.substr(0, dash), &first) || !to_i64(s.substr(dash + 1), &last) || first < 0 || last < first) return false;
    out->offset = first;
    out->length = last - first + 1;
    return true;
}

// ---- URLs --------------------------------------------------------------------------------------

struct UrlParts {
    std::string scheme, authority, path, query, fragment;
    bool has_authority = false, has_query = false, has_fragment = false;
};

UrlParts split_url(const std::string &u) {
    UrlParts p;
    size_t pos = 0;
    const size_t colon = u.find(':');
    const size_t first_delim = u.find_first_of("/?#");
    if (colon != std::string::npos && colon > 0 && (first_delim == std::string::npos || colon < first_delim) &&
        std::isalpha(static_cast<unsigned char>(u[0]))) {
        p.scheme = u.substr(0, colon);
        pos = colon + 1;
    }
    if (u.compare(pos, 2, "//") == 0) {
        p.has_authority = true;
        const size_t end = u.find_first_of("/?#", pos + 2);
        p.authority = u.substr(pos + 2, (end == std::string::npos ? u.size() : end) - pos - 2);
        pos = end == std::string::npos ? u.size() : end;
    }
    const size_t path_end = u.find_first_of("?#", pos);
    p.path = u.substr(pos, (path_end == std::string::npos ? u.size() : path_end) - pos);
    pos = path_end == std::string::npos ? u.size() : path_end;
    if (pos < u.size() && u[pos] == '?') {
        p.has_query = true;
        const size_t end = u.find('#', pos);
        p.query = u.substr(pos + 1, (end == std::string::npos ? u.size() : end) - pos - 1);
        pos = end == std::string::npos ? u.size() : end;
    }
    if (pos < u.size() && u[pos] == '#') {
        p.has_fragment = true;
        p.fragment = u.substr(pos + 1);
    }
    return p;
}

std::string remove_dot_segments(const std::string &path) {
    std::vector<std::string> out;
    size_t i = 0;
    const bool absolute = !path.empty() && path[0] == '/';
    std::string seg;
    bool trailing = false;
    while (i <= path.size()) {
        const size_t slash = path.find('/', i);
        seg = path.substr(i, (slash == std::string::npos ? path.size() : slash) - i);
        const bool last = slash == std::string::npos;
        if (seg == "..") {
            if (!out.empty()) out.pop_back();
            trailing = true;
        } else if (seg == ".") {
            trailing = true;
        } else if (!(seg.empty() && i == 0 && absolute)) {
            out.push_back(seg);
            trailing = false;
        }
        if (last) break;
        i = slash + 1;
    }
    std::string r = absolute ? "/" : "";
    for (size_t k = 0; k < out.size(); ++k) {
        if (k > 0) r += '/';
        r += out[k];
    }
    if (trailing && (r.empty() || r.back() != '/')) r += '/';
    return r;
}

std::string join_url(const UrlParts &p) {
    std::string r;
    if (!p.scheme.empty()) r += p.scheme + ":";
    if (p.has_authority) r += "//" + p.authority;
    r += p.path;
    if (p.has_query) r += "?" + p.query;
    if (p.has_fragment) r += "#" + p.fragment;
    return r;
}

// ---- DASH --------------------------------------------------------------------------------------

// SegmentTemplate/SegmentList/SegmentBase attributes inherited down the hierarchy.
struct SegmentInfo {
    std::map<std::string, std::string> attrs;
    const XmlNode *timeline = nullptr;
    const XmlNode *initialization = nullptr;  // SegmentBase/SegmentList <Initialization>
    std::vector<const XmlNode *> segment_urls;  // SegmentList
    void merge(const XmlNode *n) {
        if (n == nullptr) return;
        for (const auto &[k, v] : n->attributes) attrs[k] = v;
        if (const XmlNode *t = n->child("SegmentTimeline")) timeline = t;
        if (const XmlNode *i = n->child("Initialization")) initialization = i;
        const auto urls = n->children_named("SegmentURL");
        if (!urls.empty()) segment_urls = urls;
    }
    const std::string *get(const std::string &k) const {
        auto it = attrs.find(k);
        return it == attrs.end() ? nullptr : &it->second;
    }
};

// Expands $Identifier$ and $Identifier%0Nd$ in a SegmentTemplate URL.
std::string expand_template(const std::string &t, const std::string &rep_id, int64_t number, int64_t bandwidth, int64_t time) {
    std::string out;
    size_t i = 0;
    while (i < t.size()) {
        if (t[i] != '$') {
            out += t[i++];
            continue;
        }
        const size_t end = t.find('$', i + 1);
        if (end == std::string::npos) {
            out += t.substr(i);
            break;
        }
        const std::string tag = t.substr(i + 1, end - i - 1);
        i = end + 1;
        if (tag.empty()) {
            out += '$';
            continue;
        }
        const size_t pct = tag.find('%');
        const std::string name = tag.substr(0, pct);
        int width = 0;
        if (pct != std::string::npos) {
            const std::string fmt = tag.substr(pct + 1);  // "05d"
            width = std::atoi(fmt.c_str());
        }
        auto number_str = [width](int64_t v) {
            std::string s = std::to_string(v);
            while (static_cast<int>(s.size()) < width) s.insert(s.begin(), '0');
            return s;
        };
        if (name == "RepresentationID") {
            out += rep_id;
        } else if (name == "Number") {
            out += number_str(number);
        } else if (name == "Bandwidth") {
            out += number_str(bandwidth);
        } else if (name == "Time") {
            out += number_str(time);
        } else {
            out += "$" + tag + "$";  // unknown: left as written
        }
    }
    return out;
}

TrackType dash_type(const std::string &content_type, const std::string &mime, const std::string &codecs) {
    const std::string ct = lower(content_type);
    if (ct == "video") return TrackType::Video;
    if (ct == "audio") return TrackType::Audio;
    if (ct == "text") return TrackType::Subtitle;
    const std::string m = lower(mime);
    if (m.rfind("video/", 0) == 0) return TrackType::Video;
    if (m.rfind("audio/", 0) == 0) return TrackType::Audio;
    if (m.rfind("text/", 0) == 0 || m == "application/ttml+xml") return TrackType::Subtitle;
    if (m == "application/mp4") {
        const std::string c = lower(codecs);
        if (c.rfind("stpp", 0) == 0 || c.rfind("wvtt", 0) == 0) return TrackType::Subtitle;
    }
    if (codec_from_rfc6381(codecs, TrackType::Video) != CodecId::Unknown) return TrackType::Video;
    if (codec_from_rfc6381(codecs, TrackType::Audio) != CodecId::Unknown) return TrackType::Audio;
    return TrackType::Other;
}

bool is_dolby_vision(const std::string &codecs) {
    const std::string c = lower(codecs);
    return c.rfind("dvhe", 0) == 0 || c.rfind("dvh1", 0) == 0 || c.rfind("dav1", 0) == 0 || c.rfind("dva1", 0) == 0 ||
           c.rfind("dvav", 0) == 0;
}

// Dolby's AudioChannelConfiguration value: a 16-bit hex mask, most significant bit first
// L, C, R, Ls, Rs, Lc/Rc, Lrs/Rrs, Cs, Ts, Lsd/Rsd, Lw/Rw, Vhl/Vhr, Vhc, Lts/Rts, LFE2, LFE
// (ETSI TS 102 366 annex I), so "F801" is 5.1.
int dolby_channel_count(const std::string &hex) {
    static constexpr int kPerBit[16] = {1, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1, 2, 1, 1};
    const unsigned long mask = std::strtoul(hex.c_str(), nullptr, 16);
    int n = 0;
    for (int b = 0; b < 16; ++b) {
        if (((mask >> (15 - b)) & 1u) != 0) n += kPerBit[b];
    }
    return n;
}

std::string base_url_of(const XmlNode *n, const std::string &parent) {
    if (n == nullptr) return parent;
    const XmlNode *b = n->child("BaseURL");
    if (b == nullptr) return parent;
    const std::string t = trim(b->text);
    return t.empty() ? parent : resolve_url(parent, t);
}

// Segments of one Representation from its inherited segment information.
// What a dynamic MPD's endless series needs: elapsed time since the period began, and the window.
struct LiveClock {
    bool live = false;
    int64_t elapsed_us = kNoTime;  // now - availabilityStartTime - Period@start
    int64_t window_us = 60000000;  // timeShiftBufferDepth
};

bool dash_segments(const SegmentInfo &si, const std::string &base, const std::string &rep_id, int64_t bandwidth,
                   int64_t period_start_us, int64_t period_duration_us, bool is_template, bool is_list, Rendition *r,
                   const LiveClock &clock, std::string *error) {
    const int64_t timescale = std::max<int64_t>(1, i64_or(si.get("timescale"), 1));
    const int64_t pto = i64_or(si.get("presentationTimeOffset"), 0);
    const int64_t start_number = i64_or(si.get("startNumber"), 1);
    const int64_t duration = i64_or(si.get("duration"), 0);
    r->time_origin_us = ticks_to_us(pto, timescale) - period_start_us;

    auto add = [&](std::string url, ByteRange range, int64_t t, int64_t d, int64_t number) -> bool {
        if (r->segments.size() >= kMaxSegments) {
            *error = "too many segments";
            return false;
        }
        MediaSegment s;
        s.url = std::move(url);
        s.range = range;
        s.start_us = period_start_us + ticks_to_us(t - pto, timescale);
        s.duration_us = ticks_to_us(d, timescale);
        s.number = number;
        r->segments.push_back(std::move(s));
        return true;
    };

    if (is_template) {
        if (const std::string *init = si.get("initialization")) {
            MediaSegment s;
            s.url = resolve_url(base, expand_template(*init, rep_id, 0, bandwidth, 0));
            r->init = s;
        }
        const std::string *media = si.get("media");
        if (media == nullptr) {
            *error = "SegmentTemplate without @media";
            return false;
        }
        if (si.timeline != nullptr) {
            int64_t t = 0, number = start_number;
            const auto entries = si.timeline->children_named("S");
            for (size_t k = 0; k < entries.size(); ++k) {
                const XmlNode *e = entries[k];
                if (const std::string *ts = e->attribute("t")) to_i64(*ts, &t);
                const int64_t d = i64_or(e->attribute("d"), 0);
                if (d <= 0) {
                    *error = "SegmentTimeline entry without @d";
                    return false;
                }
                int64_t repeat = i64_or(e->attribute("r"), 0);
                if (repeat < 0) {
                    // Until the next entry's @t, else the end of the period.
                    int64_t until = -1;
                    if (k + 1 < entries.size() && entries[k + 1]->attribute("t") != nullptr) {
                        until = i64_or(entries[k + 1]->attribute("t"), -1);
                    } else if (clock.live && clock.elapsed_us != kNoTime) {
                        until = pto + clock.elapsed_us / 1000000 * timescale + (clock.elapsed_us % 1000000) * timescale / 1000000;
                    } else if (period_duration_us != kNoTime) {
                        until = pto + period_duration_us / 1000000 * timescale +
                                (period_duration_us % 1000000) * timescale / 1000000;
                    }
                    if (until < 0) {
                        *error = "open-ended SegmentTimeline without a period duration";
                        return false;
                    }
                    // Live: only segments that have fully arrived; otherwise up to the period's end.
                    const bool by_clock = clock.live && clock.elapsed_us != kNoTime &&
                                          !(k + 1 < entries.size() && entries[k + 1]->attribute("t") != nullptr);
                    repeat = until > t ? (by_clock ? (until - t) / d : (until - t + d - 1) / d) - 1 : 0;
                    if (repeat < 0) continue;  // not even the first has arrived
                }
                for (int64_t n = 0; n <= repeat; ++n) {
                    if (!add(resolve_url(base, expand_template(*media, rep_id, number, bandwidth, t)), {}, t, d, number)) return false;
                    t += d;
                    ++number;
                }
            }
            return true;
        }
        if (duration <= 0) {
            *error = "SegmentTemplate without @duration or SegmentTimeline";
            return false;
        }
        if (clock.live) {
            // An endless series: what has fully arrived by now, back to the time-shift window.
            if (clock.elapsed_us == kNoTime) {
                *error = "a live MPD needs the current time";
                return false;
            }
            const int64_t dur_us = ticks_to_us(duration, timescale);
            if (dur_us <= 0) {
                *error = "bad segment duration";
                return false;
            }
            const int64_t last = clock.elapsed_us / dur_us - 1;  // the newest complete segment
            const int64_t first = std::max<int64_t>(0, (clock.elapsed_us - clock.window_us) / dur_us);
            for (int64_t n = first; n <= last; ++n) {
                const int64_t t = pto + n * duration;
                if (!add(resolve_url(base, expand_template(*media, rep_id, start_number + n, bandwidth, t)), {}, t, duration,
                         start_number + n)) {
                    return false;
                }
            }
            return true;
        }
        if (period_duration_us == kNoTime) {
            *error = "SegmentTemplate@duration without a period duration";
            return false;
        }
        // ceil(period duration / segment duration), computed in ticks.
        const int64_t period_ticks = period_duration_us / 1000000 * timescale + (period_duration_us % 1000000) * timescale / 1000000;
        const int64_t count = std::max<int64_t>(1, (period_ticks + duration - 1) / duration);
        for (int64_t n = 0; n < count; ++n) {
            const int64_t t = pto + n * duration;
            const int64_t d = std::min(duration, period_ticks - n * duration);
            if (!add(resolve_url(base, expand_template(*media, rep_id, start_number + n, bandwidth, t)), {}, t,
                     d > 0 ? d : duration, start_number + n)) {
                return false;
            }
        }
        return true;
    }

    if (si.initialization != nullptr) {
        MediaSegment s;
        s.url = resolve_url(base, si.initialization->attribute_or("sourceURL", ""));
        if (const std::string *rg = si.initialization->attribute("range")) parse_range(*rg, &s.range);
        r->init = s;
    }

    if (is_list) {
        int64_t t = pto;
        std::vector<std::pair<int64_t, int64_t>> times;  // from a timeline, if any
        if (si.timeline != nullptr) {
            int64_t tt = 0;
            for (const XmlNode *e : si.timeline->children_named("S")) {
                if (const std::string *ts = e->attribute("t")) to_i64(*ts, &tt);
                const int64_t d = i64_or(e->attribute("d"), 0);
                const int64_t repeat = std::max<int64_t>(0, i64_or(e->attribute("r"), 0));
                for (int64_t n = 0; n <= repeat && times.size() < kMaxSegments; ++n, tt += d) times.emplace_back(tt, d);
            }
        } else if (duration <= 0 && si.segment_urls.size() > 1) {
            *error = "SegmentList without @duration or SegmentTimeline";
            return false;
        }
        for (size_t k = 0; k < si.segment_urls.size(); ++k) {
            const XmlNode *u = si.segment_urls[k];
            ByteRange range;
            if (const std::string *rg = u->attribute("mediaRange")) parse_range(*rg, &range);
            int64_t st = t, d = duration;
            if (k < times.size()) {
                st = times[k].first;
                d = times[k].second;
            } else if (duration <= 0 && period_duration_us != kNoTime) {
                d = period_duration_us * timescale / 1000000;
            }
            if (!add(resolve_url(base, u->attribute_or("media", "")), range, st, d, static_cast<int64_t>(k))) return false;
            t = st + d;
        }
        return true;
    }

    // SegmentBase: an index to read, or the whole resource as one segment.
    if (const std::string *ir = si.get("indexRange")) {
        if (!parse_range(*ir, &r->index)) {
            *error = "bad @indexRange";
            return false;
        }
        r->index_url = base;
        return true;
    }
    MediaSegment whole;
    whole.url = base;
    whole.start_us = period_start_us;
    whole.duration_us = period_duration_us != kNoTime ? period_duration_us : 0;
    r->segments.push_back(whole);
    return true;
}

// ---- HLS ---------------------------------------------------------------------------------------

// KEY=VALUE,KEY="VALUE, with commas" → map.
std::map<std::string, std::string> hls_attributes(const std::string &s) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',')) ++i;
        const size_t eq = s.find('=', i);
        if (eq == std::string::npos) break;
        const std::string key = trim(s.substr(i, eq - i));
        i = eq + 1;
        std::string value;
        if (i < s.size() && s[i] == '"') {
            const size_t close = s.find('"', i + 1);
            value = s.substr(i + 1, (close == std::string::npos ? s.size() : close) - i - 1);
            i = close == std::string::npos ? s.size() : close + 1;
        } else {
            const size_t comma = s.find(',', i);
            value = trim(s.substr(i, (comma == std::string::npos ? s.size() : comma) - i));
            i = comma == std::string::npos ? s.size() : comma;
        }
        out[key] = value;
    }
    return out;
}

std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= text.size()) {
        size_t nl = text.find('\n', i);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(i, nl - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(line);
        if (!line.empty()) out.push_back(std::move(line));
        if (nl == text.size()) break;
        i = nl + 1;
    }
    if (!out.empty() && out[0].compare(0, 3, "\xEF\xBB\xBF") == 0) out[0] = out[0].substr(3);
    return out;
}

const std::string *find(const std::map<std::string, std::string> &m, const char *k) {
    auto it = m.find(k);
    return it == m.end() ? nullptr : &it->second;
}

// EXT-X-MAP's "1234@5678" or "1234" (from the start).
// A cenc:default_KID ("2880fe36-e44b-f9bf-79d2-752e234818a5") as 32 lowercase hex digits, or "".
std::string kid_hex(const std::string &uuid) {
    std::string hex;
    for (const char c : uuid) {
        if (c == '-') continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
        hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return hex.size() == 32 ? hex : std::string();
}

// DASH ContentProtection of a Representation and its AdaptationSet: the mp4protection scheme and
// default KIDs, and a ClearKey licence URL (W3C clearkey:Laurl or DASH-IF dashif:Laurl, under the
// ClearKey system id e2719d58-a985-b3c9-781a-b030af78d30e or the mp4protection element).
void read_protection(const XmlNode *as, const XmlNode *rep, const std::string &base, Rendition *r) {
    for (const XmlNode *owner : {rep, as}) {
        for (const XmlNode *cp : owner->children_named("ContentProtection")) {
            const std::string scheme = cp->attribute_or("schemeIdUri", "");
            if (scheme == "urn:mpeg:dash:mp4protection:2011" && r->protection_scheme.empty()) {
                r->protection_scheme = cp->attribute_or("value", "");
            }
            if (const std::string *kid = cp->attribute("default_KID")) {
                const std::string hex = kid_hex(*kid);
                if (!hex.empty() && std::find(r->key_ids.begin(), r->key_ids.end(), hex) == r->key_ids.end()) {
                    r->key_ids.push_back(hex);
                }
            }
            if (r->license_url.empty()) {
                if (const XmlNode *la = cp->child("Laurl")) {
                    const std::string url = trim(la->text);
                    if (!url.empty()) r->license_url = resolve_url(base, url);
                }
            }
        }
    }
}

bool hls_byterange(const std::string &s, ByteRange *out) {
    const size_t at = s.find('@');
    int64_t len = 0, off = 0;
    if (!to_i64(s.substr(0, at), &len) || len <= 0) return false;
    if (at != std::string::npos && !to_i64(s.substr(at + 1), &off)) return false;
    out->offset = off;
    out->length = len;
    return true;
}

}  // namespace

// ---- public ------------------------------------------------------------------------------------

std::string resolve_url(const std::string &base, const std::string &ref) {
    const UrlParts r = split_url(ref);
    if (!r.scheme.empty()) {
        UrlParts t = r;
        t.path = remove_dot_segments(r.path);
        return join_url(t);
    }
    const UrlParts b = split_url(base);
    UrlParts t;
    t.scheme = b.scheme;
    if (r.has_authority) {
        t.has_authority = true;
        t.authority = r.authority;
        t.path = remove_dot_segments(r.path);
        t.has_query = r.has_query;
        t.query = r.query;
    } else {
        t.has_authority = b.has_authority;
        t.authority = b.authority;
        if (r.path.empty()) {
            t.path = b.path;
            t.has_query = r.has_query || b.has_query;
            t.query = r.has_query ? r.query : b.query;
        } else {
            if (r.path[0] == '/') {
                t.path = remove_dot_segments(r.path);
            } else {
                std::string merged;
                if (b.has_authority && b.path.empty()) {
                    merged = "/" + r.path;
                } else {
                    const size_t slash = b.path.rfind('/');
                    merged = (slash == std::string::npos ? "" : b.path.substr(0, slash + 1)) + r.path;
                }
                t.path = remove_dot_segments(merged);
            }
            t.has_query = r.has_query;
            t.query = r.query;
        }
    }
    t.has_fragment = r.has_fragment;
    t.fragment = r.fragment;
    return join_url(t);
}

bool parse_iso_duration(const std::string &str, int64_t *us) {
    const std::string s = trim(str);
    if (s.size() < 2 || s[0] != 'P') return false;
    double total = 0;
    bool in_time = false, any = false;
    size_t i = 1;
    while (i < s.size()) {
        if (s[i] == 'T') {
            in_time = true;
            ++i;
            continue;
        }
        char *end = nullptr;
        const double v = std::strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i || *end == '\0') return false;
        const char unit = *end;
        i = static_cast<size_t>(end - s.c_str()) + 1;
        any = true;
        if (!in_time) {
            switch (unit) {
                case 'Y': total += v * 365 * 86400; break;
                case 'M': total += v * 30 * 86400; break;
                case 'W': total += v * 7 * 86400; break;
                case 'D': total += v * 86400; break;
                default: return false;
            }
        } else {
            switch (unit) {
                case 'H': total += v * 3600; break;
                case 'M': total += v * 60; break;
                case 'S': total += v; break;
                default: return false;
            }
        }
    }
    if (!any || total < 0) return false;
    *us = static_cast<int64_t>(std::llround(total * 1e6));
    return true;
}

CodecId codec_from_rfc6381(const std::string &codecs, TrackType type) {
    size_t i = 0;
    while (i <= codecs.size()) {
        size_t comma = codecs.find(',', i);
        if (comma == std::string::npos) comma = codecs.size();
        const std::string c = lower(trim(codecs.substr(i, comma - i)));
        const std::string fourcc = c.substr(0, c.find('.'));
        CodecId id = CodecId::Unknown;
        if (fourcc == "avc1" || fourcc == "avc3") id = CodecId::H264;
        else if (fourcc == "hvc1" || fourcc == "hev1" || fourcc == "dvh1" || fourcc == "dvhe") id = CodecId::Hevc;
        else if (fourcc == "vp09" || fourcc == "vp9") id = CodecId::Vp9;
        else if (fourcc == "av01" || fourcc == "dav1") id = CodecId::Av1;
        else if (fourcc == "ac-3") id = CodecId::Ac3;
        else if (fourcc == "ec-3") id = CodecId::Eac3;
        else if (fourcc == "opus") id = CodecId::Opus;
        else if (fourcc == "mp3" || c == "mp4a.40.34" || c == "mp4a.6b" || c == "mp4a.69") id = CodecId::Mp3;
        else if (fourcc == "mp4a") id = CodecId::Aac;
        if (id != CodecId::Unknown && is_video(id) == (type == TrackType::Video)) return id;
        if (comma == codecs.size()) break;
        i = comma + 1;
    }
    return CodecId::Unknown;
}

bool parse_iso_datetime(const std::string &str, int64_t *epoch_us) {
    // YYYY-MM-DDThh:mm:ss[.fraction][Z|+hh:mm|-hh:mm]; a missing zone is UTC (DASH profiles say so).
    const std::string s = trim(str);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    double sec = 0;
    int n = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%lf%n", &y, &mo, &d, &h, &mi, &sec, &n) < 6 || mo < 1 || mo > 12 ||
        d < 1 || d > 31 || h > 23 || mi > 59 || sec < 0 || sec >= 61) {
        return false;
    }
    int64_t zone_s = 0;
    const std::string zone = s.substr(static_cast<size_t>(n));
    if (!zone.empty() && zone != "Z") {
        int zh = 0, zm = 0;
        if ((zone[0] != '+' && zone[0] != '-') || std::sscanf(zone.c_str() + 1, "%2d:%2d", &zh, &zm) != 2) return false;
        zone_s = (zone[0] == '+' ? 1 : -1) * (zh * 3600 + zm * 60);
    }
    // Days from the civil date (Howard Hinnant's algorithm).
    const int yy = y - (mo <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const int yoe = yy - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
    const int64_t whole = days * 86400 + h * 3600 + mi * 60 - zone_s;
    *epoch_us = whole * 1000000 + static_cast<int64_t>(std::llround(sec * 1e6));
    return true;
}

int dolby_vision_profile(const std::string &codec) {
    if (!is_dolby_vision(codec)) return -1;
    const size_t dot = codec.find('.');
    if (dot == std::string::npos || dot + 1 >= codec.size()) return -1;
    char *end = nullptr;
    const long p = std::strtol(codec.c_str() + dot + 1, &end, 10);
    return end != codec.c_str() + dot + 1 && p >= 0 && p < 100 ? static_cast<int>(p) : -1;
}

bool parse_mpd(const std::string &doc, const std::string &url, Presentation *out, std::string *error, int64_t now_epoch_us) {
    *out = Presentation{};
    out->format = Presentation::Format::Dash;
    out->url = url;
    XmlNode mpd;
    if (!parse_xml(doc, &mpd, error)) {
        *error = "MPD: " + *error;
        return false;
    }
    if (mpd.local() != "MPD") {
        *error = "not an MPD (root element " + mpd.name + ")";
        return false;
    }
    out->live = mpd.attribute_or("type", "static") == "dynamic";
    auto duration_attr = [&](const char *name) {
        int64_t v = kNoTime;
        if (const std::string *d = mpd.attribute(name)) {
            if (!parse_iso_duration(*d, &v)) v = kNoTime;
        }
        return v;
    };
    if (const std::string *ast = mpd.attribute("availabilityStartTime")) parse_iso_datetime(*ast, &out->availability_start_us);
    out->update_period_us = duration_attr("minimumUpdatePeriod");
    out->time_shift_us = duration_attr("timeShiftBufferDepth");
    out->live_delay_us = duration_attr("suggestedPresentationDelay");
    if (const XmlNode *utc = mpd.child("UTCTiming")) {
        out->utc_timing_scheme = utc->attribute_or("schemeIdUri", "");
        out->utc_timing_value = utc->attribute_or("value", "");
    }
    int64_t mpd_duration = kNoTime;
    if (const std::string *d = mpd.attribute("mediaPresentationDuration")) {
        int64_t v = 0;
        if (parse_iso_duration(*d, &v)) mpd_duration = v;
    }
    const XmlNode *period = mpd.child("Period");
    if (period == nullptr) {
        *error = "MPD without a Period";
        return false;
    }
    int64_t period_start = 0, period_duration = kNoTime;
    if (const std::string *s = period->attribute("start")) parse_iso_duration(*s, &period_start);
    if (const std::string *d = period->attribute("duration")) {
        int64_t v = 0;
        if (parse_iso_duration(*d, &v)) period_duration = v;
    }
    if (period_duration == kNoTime && mpd_duration != kNoTime) period_duration = mpd_duration - period_start;
    out->duration_us = mpd_duration != kNoTime ? mpd_duration : period_duration;
    LiveClock clock;
    clock.live = out->live;
    if (out->live) {
        if (out->time_shift_us != kNoTime) clock.window_us = out->time_shift_us;
        if (now_epoch_us != kNoTime) {
            const int64_t ast = out->availability_start_us != kNoTime ? out->availability_start_us : 0;
            clock.elapsed_us = now_epoch_us - ast - period_start;
        }
        out->duration_us = kNoTime;
    }

    const std::string mpd_base = base_url_of(&mpd, url);
    const std::string period_base = base_url_of(period, mpd_base);
    SegmentInfo period_si;
    bool period_template = false, period_list = false;
    if (const XmlNode *n = period->child("SegmentTemplate")) period_si.merge(n), period_template = true;
    if (const XmlNode *n = period->child("SegmentList")) period_si.merge(n), period_list = true;
    if (const XmlNode *n = period->child("SegmentBase")) period_si.merge(n);

    int as_index = 0;
    for (const XmlNode *as : period->children_named("AdaptationSet")) {
        const std::string as_base = base_url_of(as, period_base);
        SegmentInfo as_si = period_si;
        bool as_template = period_template, as_list = period_list;
        if (const XmlNode *n = as->child("SegmentTemplate")) as_si.merge(n), as_template = true;
        if (const XmlNode *n = as->child("SegmentList")) as_si.merge(n), as_list = true;
        if (const XmlNode *n = as->child("SegmentBase")) as_si.merge(n);
        const bool as_protected = as->child("ContentProtection") != nullptr;
        bool as_atmos = false;
        for (const char *prop : {"SupplementalProperty", "EssentialProperty"}) {
            for (const XmlNode *p : as->children_named(prop)) {
                if (p->attribute_or("schemeIdUri", "").find("EC3_ExtensionType") != std::string::npos &&
                    p->attribute_or("value", "") == "JOC") {
                    as_atmos = true;
                }
            }
        }
        bool as_main = false;
        for (const XmlNode *role : as->children_named("Role")) as_main = as_main || role->attribute_or("value", "") == "main";
        const std::string as_id = as->attribute_or("id", std::to_string(as_index++));
        for (const XmlNode *rep : as->children_named("Representation")) {
            Rendition r;
            r.id = rep->attribute_or("id", "");
            r.group = as_id;
            r.codecs = rep->attribute_or("codecs", as->attribute_or("codecs", ""));
            r.mime_type = rep->attribute_or("mimeType", as->attribute_or("mimeType", ""));
            r.type = dash_type(as->attribute_or("contentType", ""), r.mime_type, r.codecs);
            r.bandwidth = i64_or(rep->attribute("bandwidth"), 0);
            r.width = static_cast<int>(i64_or(rep->attribute("width"), i64_or(as->attribute("width"), 0)));
            r.height = static_cast<int>(i64_or(rep->attribute("height"), i64_or(as->attribute("height"), 0)));
            r.frame_rate = frame_rate(rep->attribute_or("frameRate", as->attribute_or("frameRate", "")));
            r.sample_rate = static_cast<int>(i64_or(rep->attribute("audioSamplingRate"), i64_or(as->attribute("audioSamplingRate"), 0)));
            const XmlNode *acc = rep->child("AudioChannelConfiguration");
            if (acc == nullptr) acc = as->child("AudioChannelConfiguration");
            if (acc != nullptr) {
                const std::string value = acc->attribute_or("value", "");
                int64_t ch = 0;
                if (acc->attribute_or("schemeIdUri", "").find("dolby") != std::string::npos) {
                    r.channels = dolby_channel_count(value);
                } else if (to_i64(value, &ch)) {
                    r.channels = static_cast<int>(ch);
                }
            }
            r.language = rep->attribute_or("lang", as->attribute_or("lang", ""));
            if (const XmlNode *label = as->child("Label")) r.label = trim(label->text);
            r.is_default = as_main;
            r.encrypted = as_protected || rep->child("ContentProtection") != nullptr;
            if (r.encrypted) read_protection(as, rep, base_url_of(rep, as_base), &r);
            if (is_dolby_vision(r.codecs)) r.dolby_vision = r.codecs;
            r.atmos = as_atmos;
            for (const char *prop : {"SupplementalProperty", "EssentialProperty"}) {
                for (const XmlNode *p : rep->children_named(prop)) {
                    if (p->attribute_or("schemeIdUri", "").find("EC3_ExtensionType") != std::string::npos &&
                        p->attribute_or("value", "") == "JOC") {
                        r.atmos = true;
                    }
                }
            }
            SegmentInfo si = as_si;
            bool is_template = as_template, is_list = as_list;
            if (const XmlNode *n = rep->child("SegmentTemplate")) si.merge(n), is_template = true;
            if (const XmlNode *n = rep->child("SegmentList")) si.merge(n), is_list = true, is_template = false;
            if (const XmlNode *n = rep->child("SegmentBase")) si.merge(n), is_template = false, is_list = false;
            const std::string base = base_url_of(rep, as_base);
            std::string why;
            if (!dash_segments(si, base, r.id, r.bandwidth, period_start, period_duration, is_template, is_list, &r, clock,
                               &why)) {
                *error = "Representation " + r.id + ": " + why;
                return false;
            }
            out->renditions.push_back(std::move(r));
        }
    }
    if (out->renditions.empty()) {
        *error = "MPD without Representations";
        return false;
    }
    return true;
}

bool parse_hls(const std::string &text, const std::string &url, Presentation *out, std::string *error) {
    *out = Presentation{};
    out->format = Presentation::Format::Hls;
    out->url = url;
    const std::vector<std::string> lines = lines_of(text);
    if (lines.empty() || lines[0] != "#EXTM3U") {
        *error = "not an HLS playlist (no #EXTM3U)";
        return false;
    }
    const bool master = std::any_of(lines.begin(), lines.end(),
                                    [](const std::string &l) { return l.rfind("#EXT-X-STREAM-INF:", 0) == 0; });
    if (!master) {
        Rendition r;
        r.id = "0";
        r.type = TrackType::Video;
        r.muxed_audio = true;
        r.playlist_url = url;
        bool live = false;
        if (!parse_hls_media(text, url, &r, &live, error)) return false;
        out->live = live;
        int64_t total = 0;
        for (const MediaSegment &s : r.segments) total += s.duration_us;
        out->duration_us = live ? kNoTime : total;
        out->renditions.push_back(std::move(r));
        return true;
    }

    std::vector<Rendition> media;
    std::map<std::string, bool> group_has_uri;  // audio GROUP-ID → some rendition has its own playlist
    int index = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &l = lines[i];
        if (l.rfind("#EXT-X-MEDIA:", 0) == 0) {
            const auto a = hls_attributes(l.substr(13));
            const std::string type = find(a, "TYPE") != nullptr ? *find(a, "TYPE") : "";
            if (type != "AUDIO" && type != "SUBTITLES") continue;
            const std::string *uri = find(a, "URI");
            const std::string group = find(a, "GROUP-ID") != nullptr ? *find(a, "GROUP-ID") : "";
            if (type == "AUDIO") group_has_uri[group] = group_has_uri[group] || uri != nullptr;
            if (uri == nullptr) continue;  // carried in the variant's own segments
            Rendition r;
            r.id = "m" + std::to_string(index++);
            r.type = type == "AUDIO" ? TrackType::Audio : TrackType::Subtitle;
            r.group = group;
            if (const std::string *v = find(a, "LANGUAGE")) r.language = *v;
            if (const std::string *v = find(a, "NAME")) r.label = *v;
            r.is_default = find(a, "DEFAULT") != nullptr && *find(a, "DEFAULT") == "YES";
            if (const std::string *v = find(a, "CHANNELS")) {
                r.channels = std::atoi(v->c_str());
                r.atmos = v->find("/JOC") != std::string::npos;
            }
            r.playlist_url = resolve_url(url, *uri);
            media.push_back(std::move(r));
        } else if (l.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
            const auto a = hls_attributes(l.substr(18));
            size_t j = i + 1;
            while (j < lines.size() && lines[j][0] == '#') ++j;
            if (j >= lines.size()) {
                *error = "EXT-X-STREAM-INF without a URI";
                return false;
            }
            Rendition r;
            r.id = "v" + std::to_string(index++);
            if (const std::string *v = find(a, "CODECS")) r.codecs = *v;
            if (const std::string *v = find(a, "BANDWIDTH")) to_i64(*v, &r.bandwidth);
            if (const std::string *v = find(a, "RESOLUTION")) {
                const size_t x = v->find('x');
                if (x != std::string::npos) {
                    r.width = std::atoi(v->substr(0, x).c_str());
                    r.height = std::atoi(v->substr(x + 1).c_str());
                }
            }
            if (const std::string *v = find(a, "FRAME-RATE")) r.frame_rate = to_double(*v);
            if (const std::string *v = find(a, "AUDIO")) r.audio_group = *v;
            const bool has_video = r.codecs.empty() || r.width > 0 ||
                                   codec_from_rfc6381(r.codecs, TrackType::Video) != CodecId::Unknown;
            r.type = has_video ? TrackType::Video : TrackType::Audio;
            if (is_dolby_vision(r.codecs)) r.dolby_vision = r.codecs.substr(0, r.codecs.find(','));
            r.playlist_url = resolve_url(url, lines[j]);
            media.push_back(std::move(r));
            i = j;
        }
    }
    // A variant carries audio itself unless its audio group has renditions with their own playlists.
    for (Rendition &r : media) {
        if (r.type == TrackType::Video) {
            const auto g = group_has_uri.find(r.audio_group);
            r.muxed_audio = r.audio_group.empty() || g == group_has_uri.end() || !g->second;
        }
    }
    // Audio renditions declare no codecs; take the audio codec of a variant that uses the group.
    for (Rendition &r : media) {
        if (r.type != TrackType::Audio || !r.codecs.empty()) continue;
        for (const Rendition &v : media) {
            if (v.type == TrackType::Video && v.audio_group == r.group && !v.codecs.empty()) {
                size_t i = 0;
                while (i <= v.codecs.size()) {
                    size_t comma = v.codecs.find(',', i);
                    if (comma == std::string::npos) comma = v.codecs.size();
                    const std::string c = trim(v.codecs.substr(i, comma - i));
                    if (codec_from_rfc6381(c, TrackType::Audio) != CodecId::Unknown) {
                        r.codecs = c;
                        break;
                    }
                    i = comma + 1;
                }
                if (!r.codecs.empty()) break;
            }
        }
    }
    out->renditions = std::move(media);
    if (out->renditions.empty()) {
        *error = "master playlist without variants";
        return false;
    }
    return true;
}

bool parse_hls_media(const std::string &text, const std::string &url, Rendition *r, bool *live, std::string *error) {
    const std::vector<std::string> lines = lines_of(text);
    if (lines.empty() || lines[0] != "#EXTM3U") {
        *error = "not an HLS playlist (no #EXTM3U)";
        return false;
    }
    r->segments.clear();
    r->init.reset();
    r->encrypted = false;
    *live = true;
    int64_t t = 0;
    int64_t sequence = 0;  // EXT-X-MEDIA-SEQUENCE: the first segment's number
    double pending_duration = -1;
    bool pending_discontinuity = false;
    ByteRange pending_range;
    bool has_range = false;
    std::map<std::string, int64_t> next_offset;  // per URI, for EXT-X-BYTERANGE without @offset
    for (const std::string &l : lines) {
        if (l.rfind("#EXTINF:", 0) == 0) {
            pending_duration = to_double(l.substr(8));
        } else if (l.rfind("#EXT-X-BYTERANGE:", 0) == 0) {
            has_range = true;
            pending_range = ByteRange{};
            const std::string v = l.substr(17);
            const size_t at = v.find('@');
            int64_t len = 0, off = -1;  // no @offset: continues after the previous range of the URI
            to_i64(v.substr(0, at), &len);
            if (at != std::string::npos) to_i64(v.substr(at + 1), &off);
            pending_range.length = len;
            pending_range.offset = off;
        } else if (l == "#EXT-X-DISCONTINUITY") {
            pending_discontinuity = true;
        } else if (l.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
            to_i64(l.substr(22), &sequence);
        } else if (l.rfind("#EXT-X-TARGETDURATION:", 0) == 0) {
            r->target_duration_us = static_cast<int64_t>(std::llround(to_double(l.substr(22)) * 1e6));
        } else if (l == "#EXT-X-ENDLIST") {
            *live = false;
        } else if (l.rfind("#EXT-X-PLAYLIST-TYPE:", 0) == 0) {
            if (l.substr(21) == "VOD") *live = false;
        } else if (l.rfind("#EXT-X-KEY:", 0) == 0) {
            const auto a = hls_attributes(l.substr(11));
            const std::string *m = find(a, "METHOD");
            if (m != nullptr && *m != "NONE") r->encrypted = true;
        } else if (l.rfind("#EXT-X-MAP:", 0) == 0) {
            const auto a = hls_attributes(l.substr(11));
            const std::string *uri = find(a, "URI");
            if (uri == nullptr) {
                *error = "EXT-X-MAP without URI";
                return false;
            }
            MediaSegment init;
            init.url = resolve_url(url, *uri);
            if (const std::string *br = find(a, "BYTERANGE")) {
                if (!hls_byterange(*br, &init.range)) {
                    *error = "bad EXT-X-MAP BYTERANGE";
                    return false;
                }
            }
            r->init = init;
        } else if (l[0] != '#') {
            if (pending_duration < 0) {
                *error = "segment URI without EXTINF";
                return false;
            }
            if (r->segments.size() >= kMaxSegments) {
                *error = "too many segments";
                return false;
            }
            MediaSegment s;
            s.url = resolve_url(url, l);
            if (has_range) {
                const int64_t off = pending_range.offset >= 0 ? pending_range.offset : next_offset[s.url];
                if (pending_range.length <= 0) {
                    *error = "bad EXT-X-BYTERANGE";
                    return false;
                }
                s.range = ByteRange{off, pending_range.length};
                next_offset[s.url] = off + pending_range.length;
            }
            s.start_us = t;
            s.duration_us = static_cast<int64_t>(std::llround(pending_duration * 1e6));
            s.discontinuity = pending_discontinuity;
            s.number = sequence + static_cast<int64_t>(r->segments.size());
            t += s.duration_us;
            r->segments.push_back(std::move(s));
            pending_duration = -1;
            pending_discontinuity = false;
            has_range = false;
        }
    }
    if (r->segments.empty()) {
        *error = "media playlist without segments";
        return false;
    }
    return true;
}

bool parse_sidx(const uint8_t *d, size_t size, int64_t box_offset, const std::string &url,
                std::vector<MediaSegment> *out, std::string *error) {
    auto u32 = [d](size_t at) {
        return (static_cast<uint32_t>(d[at]) << 24) | (static_cast<uint32_t>(d[at + 1]) << 16) |
               (static_cast<uint32_t>(d[at + 2]) << 8) | d[at + 3];
    };
    auto u64 = [&u32](size_t at) { return (static_cast<uint64_t>(u32(at)) << 32) | u32(at + 4); };
    // Skip boxes before the sidx (an index range sometimes starts at 'styp' or similar).
    size_t at = 0;
    for (;;) {
        if (at + 8 > size) {
            *error = "no sidx box in the index range";
            return false;
        }
        const uint32_t box = u32(at);
        if (std::memcmp(d + at + 4, "sidx", 4) == 0) break;
        if (box < 8) {
            *error = "bad box in the index range";
            return false;
        }
        at += box;
    }
    const uint64_t box_size = u32(at);
    if (box_size < 32 || at + box_size > size) {
        *error = "truncated sidx";
        return false;
    }
    const uint8_t version = d[at + 8];
    size_t p = at + 12;
    p += 4;  // reference_ID
    const uint32_t timescale = u32(p);
    p += 4;
    uint64_t earliest = 0, first_offset = 0;
    if (version == 0) {
        earliest = u32(p);
        first_offset = u32(p + 4);
        p += 8;
    } else {
        if (p + 16 > at + box_size) {
            *error = "truncated sidx";
            return false;
        }
        earliest = u64(p);
        first_offset = u64(p + 8);
        p += 16;
    }
    p += 2;  // reserved
    const uint16_t count = static_cast<uint16_t>((d[p] << 8) | d[p + 1]);
    p += 2;
    if (timescale == 0 || p + static_cast<size_t>(count) * 12 > at + box_size) {
        *error = "bad sidx";
        return false;
    }
    int64_t offset = box_offset + static_cast<int64_t>(at) + static_cast<int64_t>(box_size) + static_cast<int64_t>(first_offset);
    int64_t t = static_cast<int64_t>(earliest);
    out->clear();
    for (uint16_t k = 0; k < count; ++k, p += 12) {
        const uint32_t ref = u32(p);
        if ((ref & 0x80000000u) != 0) {
            *error = "hierarchical sidx not supported";
            return false;
        }
        const int64_t len = ref & 0x7FFFFFFFu;
        const int64_t dur = u32(p + 4);
        MediaSegment s;
        s.url = url;
        s.range = ByteRange{offset, len};
        s.start_us = ticks_to_us(t, timescale);
        s.duration_us = ticks_to_us(dur, timescale);
        s.number = k;
        out->push_back(std::move(s));
        offset += len;
        t += dur;
    }
    return true;
}

}  // namespace wnk::media
