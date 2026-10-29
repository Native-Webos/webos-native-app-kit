// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/webvtt.hpp"

#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace wnk::media {
namespace {

std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            lines.push_back(cur);
            cur.clear();
        } else if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    lines.push_back(cur);
    return lines;
}

bool blank(const std::string &l) { return l.find_first_not_of(" \t") == std::string::npos; }

std::string plain(const std::string &in);

}  // namespace

std::string webvtt_plain_text(const std::string &cue_text) {
    std::string out;
    size_t i = 0;
    while (i <= cue_text.size()) {
        size_t nl = cue_text.find('\n', i);
        if (nl == std::string::npos) nl = cue_text.size();
        std::string line = plain(cue_text.substr(i, nl - i));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) out += (out.empty() ? "" : "\n") + line;
        i = nl + 1;
    }
    return out;
}

namespace {

// Cue text → plain text: tags removed, character references decoded.
std::string plain(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '<') {
            const size_t close = in.find('>', i);
            if (close == std::string::npos) break;
            i = close;
            continue;
        }
        if (c == '&') {
            static const struct {
                const char *name;
                const char *text;
            } kRefs[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}, {"&lrm;", ""}, {"&rlm;", ""},
                         {"&quot;", "\""}, {"&apos;", "'"}};
            bool hit = false;
            for (const auto &r : kRefs) {
                const std::string n = r.name;
                if (in.compare(i, n.size(), n) == 0) {
                    out += r.text;
                    i += n.size() - 1;
                    hit = true;
                    break;
                }
            }
            if (hit) continue;
        }
        out += c;
    }
    return out;
}

}  // namespace

bool parse_vtt_timestamp(const std::string &s, int64_t *us) {
    // [hh:]mm:ss.ttt with hh of any width.
    int64_t parts[3] = {0, 0, 0};
    int n = 0;
    size_t i = 0;
    while (i < s.size() && n < 3) {
        size_t j = i;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
        if (j == i) return false;
        parts[n++] = std::atoll(s.substr(i, j - i).c_str());
        i = j;
        if (i < s.size() && s[i] == ':') {
            ++i;
            continue;
        }
        break;
    }
    if (i >= s.size() || s[i] != '.' || i + 4 > s.size()) return false;
    for (size_t k = i + 1; k < i + 4; ++k) {
        if (s[k] < '0' || s[k] > '9') return false;
    }
    const int64_t ms = std::atoll(s.substr(i + 1, 3).c_str());
    int64_t h = 0, m = 0, sec = 0;
    if (n == 3) {
        h = parts[0], m = parts[1], sec = parts[2];
    } else if (n == 2) {
        m = parts[0], sec = parts[1];
    } else {
        return false;
    }
    if (m > 59 || sec > 59) return false;
    *us = ((h * 60 + m) * 60 + sec) * 1000000 + ms * 1000;
    return true;
}

bool parse_webvtt(const std::string &text, WebVttFile *out, std::string *error) {
    *out = WebVttFile{};
    std::string body = text;
    if (body.compare(0, 3, "\xEF\xBB\xBF") == 0) body = body.substr(3);
    const std::vector<std::string> lines = split_lines(body);
    if (lines.empty() || lines[0].compare(0, 6, "WEBVTT") != 0 ||
        (lines[0].size() > 6 && lines[0][6] != ' ' && lines[0][6] != '\t')) {
        *error = "not WebVTT";
        return false;
    }
    size_t i = 1;
    // Header lines up to the first blank line (X-TIMESTAMP-MAP lives here).
    for (; i < lines.size() && !blank(lines[i]); ++i) {
        const std::string &l = lines[i];
        if (l.compare(0, 16, "X-TIMESTAMP-MAP=") != 0) continue;
        const size_t mp = l.find("MPEGTS:"), lo = l.find("LOCAL:");
        if (mp == std::string::npos || lo == std::string::npos) continue;
        int64_t local = 0;
        const size_t lend = l.find(',', lo);
        if (!parse_vtt_timestamp(l.substr(lo + 6, lend == std::string::npos ? std::string::npos : lend - lo - 6), &local)) continue;
        out->has_timestamp_map = true;
        out->map_mpegts = std::atoll(l.c_str() + mp + 7);
        out->map_local_us = local;
    }
    while (i < lines.size()) {
        while (i < lines.size() && blank(lines[i])) ++i;
        if (i >= lines.size()) break;
        // A block: optional identifier line, then the timing line.
        size_t t = i;
        if (lines[t].find("-->") == std::string::npos) {
            const std::string &first = lines[t];
            if (first.compare(0, 4, "NOTE") == 0 || first.compare(0, 5, "STYLE") == 0 || first.compare(0, 6, "REGION") == 0) {
                while (i < lines.size() && !blank(lines[i])) ++i;
                continue;
            }
            ++t;  // cue identifier
        }
        if (t >= lines.size() || lines[t].find("-->") == std::string::npos) {
            while (i < lines.size() && !blank(lines[i])) ++i;
            continue;
        }
        const std::string &timing = lines[t];
        const size_t arrow = timing.find("-->");
        const std::string a = timing.substr(0, arrow);
        std::string b = timing.substr(arrow + 3);
        auto trim = [](std::string s) {
            const size_t f = s.find_first_not_of(" \t");
            if (f == std::string::npos) return std::string();
            s = s.substr(f);
            const size_t e = s.find_first_of(" \t");
            return e == std::string::npos ? s : s.substr(0, e);  // drop cue settings
        };
        int64_t start = 0, end = 0;
        const bool ok = parse_vtt_timestamp(trim(a), &start) && parse_vtt_timestamp(trim(b), &end) && end >= start;
        std::string cue_text;
        size_t k = t + 1;
        for (; k < lines.size() && !blank(lines[k]); ++k) {
            const std::string line = plain(lines[k]);
            if (line.empty()) continue;
            if (!cue_text.empty()) cue_text += '\n';
            cue_text += line;
        }
        i = k;
        if (!ok || cue_text.empty()) continue;
        SubtitleCue c;
        c.start_us = start;
        c.end_us = end;
        c.text = cue_text;
        out->cues.push_back(std::move(c));
    }
    return true;
}

// ---- ISO BMFF (wvtt) ---------------------------------------------------------------------------

namespace {

uint32_t be32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint64_t be64(const uint8_t *p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }

struct Box {
    char type[5] = {};
    const uint8_t *begin = nullptr;  // the box itself
    const uint8_t *body = nullptr;   // after the header
    const uint8_t *end = nullptr;
};

// Boxes in [p, end); false on a malformed size.
bool boxes(const uint8_t *p, const uint8_t *end, std::vector<Box> *out) {
    out->clear();
    while (p < end) {
        if (end - p < 8) return false;
        uint64_t size = be32(p);
        size_t header = 8;
        if (size == 1) {
            if (end - p < 16) return false;
            size = be64(p + 8);
            header = 16;
        } else if (size == 0) {
            size = static_cast<uint64_t>(end - p);
        }
        if (size < header || size > static_cast<uint64_t>(end - p)) return false;
        Box b;
        for (int i = 0; i < 4; ++i) b.type[i] = static_cast<char>(p[4 + i]);
        b.begin = p;
        b.body = p + header;
        b.end = p + size;
        out->push_back(b);
        p += size;
    }
    return true;
}

const Box *find(const std::vector<Box> &v, const char *type) {
    for (const Box &b : v) {
        if (std::memcmp(b.type, type, 4) == 0) return &b;
    }
    return nullptr;
}

// The first box found along a path of nested container types.
bool descend(const uint8_t *p, const uint8_t *end, std::initializer_list<const char *> path, Box *out) {
    std::vector<Box> level;
    for (const char *type : path) {
        if (!boxes(p, end, &level)) return false;
        const Box *b = find(level, type);
        if (b == nullptr) return false;
        *out = *b;
        p = b->body;
        end = b->end;
    }
    return true;
}

}  // namespace

bool parse_wvtt_init(const uint8_t *data, size_t size, uint32_t *timescale, std::string *error) {
    Box mdhd;
    if (!descend(data, data + size, {"moov", "trak", "mdia", "mdhd"}, &mdhd) || mdhd.end - mdhd.body < 24) {
        *error = "no mdhd in the init segment";
        return false;
    }
    const uint8_t version = mdhd.body[0];
    const uint8_t *ts = mdhd.body + 4 + (version == 1 ? 16 : 8);
    if (ts + 4 > mdhd.end || be32(ts) == 0) {
        *error = "bad mdhd";
        return false;
    }
    *timescale = be32(ts);
    return true;
}

bool mp4_text_samples(const uint8_t *data, size_t size, uint32_t timescale, std::vector<Mp4TextSample> *out,
                      std::string *error) {
    std::vector<Box> top;
    if (timescale == 0 || !boxes(data, data + size, &top)) {
        *error = "malformed fragment";
        return false;
    }
    auto to_us = [timescale](uint64_t t) {
        return static_cast<int64_t>(t / timescale) * 1000000 + static_cast<int64_t>(t % timescale) * 1000000 / timescale;
    };
    for (const Box &moof : top) {
        if (std::memcmp(moof.type, "moof", 4) != 0) continue;
        std::vector<Box> trafs;
        if (!boxes(moof.body, moof.end, &trafs)) continue;
        for (const Box &traf : trafs) {
            if (std::memcmp(traf.type, "traf", 4) != 0) continue;
            std::vector<Box> kids;
            if (!boxes(traf.body, traf.end, &kids)) continue;
            const Box *tfhd = find(kids, "tfhd");
            const Box *tfdt = find(kids, "tfdt");
            if (tfhd == nullptr || tfhd->end - tfhd->body < 8) continue;
            const uint32_t hflags = be32(tfhd->body) & 0xFFFFFF;
            const uint8_t *h = tfhd->body + 8;  // after version/flags and track_ID
            uint32_t default_duration = 0, default_size = 0;
            if ((hflags & 0x1) != 0) h += 8;  // base_data_offset: an absolute file offset, not usable on a segment
            if ((hflags & 0x2) != 0) h += 4;
            if ((hflags & 0x8) != 0 && h + 4 <= tfhd->end) default_duration = be32(h), h += 4;
            if ((hflags & 0x10) != 0 && h + 4 <= tfhd->end) default_size = be32(h), h += 4;
            uint64_t t = 0;
            if (tfdt != nullptr && tfdt->end - tfdt->body >= 8) {
                t = tfdt->body[0] == 1 && tfdt->end - tfdt->body >= 12 ? be64(tfdt->body + 4) : be32(tfdt->body + 4);
            }
            for (const Box &trun : kids) {
                if (std::memcmp(trun.type, "trun", 4) != 0 || trun.end - trun.body < 8) continue;
                const uint32_t flags = be32(trun.body) & 0xFFFFFF;
                const uint32_t count = be32(trun.body + 4);
                const uint8_t *q = trun.body + 8;
                int64_t data_offset = 0;
                if ((flags & 0x1) != 0) {
                    data_offset = static_cast<int32_t>(be32(q));
                    q += 4;
                }
                if ((flags & 0x4) != 0) q += 4;
                const uint8_t *sample = moof.begin + data_offset;  // relative to the moof
                for (uint32_t k = 0; k < count; ++k) {
                    uint32_t dur = default_duration, sz = default_size;
                    if ((flags & 0x100) != 0) {
                        if (q + 4 > trun.end) break;
                        dur = be32(q), q += 4;
                    }
                    if ((flags & 0x200) != 0) {
                        if (q + 4 > trun.end) break;
                        sz = be32(q), q += 4;
                    }
                    if ((flags & 0x400) != 0) q += 4;
                    if ((flags & 0x800) != 0) q += 4;
                    if (sample < data || sample + sz > data + size) {
                        *error = "sample outside the fragment";
                        return false;
                    }
                    out->push_back(Mp4TextSample{to_us(t), to_us(t + dur), sample, sz});
                    sample += sz;
                    t += dur;
                }
            }
        }
    }
    return true;
}

bool parse_wvtt_fragment(const uint8_t *data, size_t size, uint32_t timescale, std::vector<SubtitleCue> *out,
                         std::string *error) {
    std::vector<Mp4TextSample> samples;
    if (!mp4_text_samples(data, size, timescale, &samples, error)) return false;
    for (const Mp4TextSample &s : samples) {
        std::vector<Box> cues;
        if (!boxes(s.data, s.data + s.size, &cues)) continue;
        for (const Box &c : cues) {
            if (std::memcmp(c.type, "vttc", 4) != 0) continue;  // vtte: nothing showing
            Box payl;
            if (!descend(c.body, c.end, {"payl"}, &payl)) continue;
            const std::string text = webvtt_plain_text(
                    std::string(reinterpret_cast<const char *>(payl.body), static_cast<size_t>(payl.end - payl.body)));
            if (text.empty()) continue;
            SubtitleCue cue;
            cue.start_us = s.start_us;
            cue.end_us = s.end_us;
            cue.text = text;
            out->push_back(std::move(cue));
        }
    }
    return true;
}

}  // namespace wnk::media
