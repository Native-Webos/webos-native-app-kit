// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/ttml.hpp"

#include "wnk/media/webvtt.hpp"
#include "wnk/media/xml.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace wnk::media {
namespace {

constexpr int64_t kOpen = std::numeric_limits<int64_t>::max();  // no end known

bool digits(const std::string &s, size_t from, size_t to) {
    if (from >= to) return false;
    for (size_t i = from; i < to; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

// "12" or "12.5" (digits, optionally a fraction) → value; false otherwise.
bool decimal(const std::string &s, double *v) {
    const size_t dot = s.find('.');
    if (!digits(s, 0, dot == std::string::npos ? s.size() : dot)) return false;
    if (dot != std::string::npos && !digits(s, dot + 1, s.size())) return false;
    *v = std::strtod(s.c_str(), nullptr);
    return true;
}

double attr_number(const XmlNode &n, const char *name, double fallback) {
    const std::string *v = n.attribute(name);
    double d = 0;
    return v != nullptr && decimal(*v, &d) && d > 0 ? d : fallback;
}

struct Fragment {
    int64_t begin = 0, end = kOpen;
    std::string text;  // white space already treated
    bool line_break = false;
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string collapse(const std::string &s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (is_space(c)) {
            space = true;
            continue;
        }
        if (space) out += ' ';  // also at the start and the end: joining runs decides
        space = false;
        out += c;
    }
    if (space) out += ' ';
    return out;
}

class Reader {
 public:
    explicit Reader(TtmlFile *out) : out_(out) {}

    bool run(const XmlNode &tt, std::string *error) {
        const double frame_rate = attr_number(tt, "frameRate", 30);
        double multiplier = 1;
        if (const std::string *m = tt.attribute("frameRateMultiplier")) {
            const size_t sp = m->find(' ');
            double num = 0, den = 0;
            if (sp != std::string::npos && decimal(m->substr(0, sp), &num) && decimal(m->substr(sp + 1), &den) && num > 0 &&
                den > 0) {
                multiplier = num / den;
            }
        }
        rates_.frame_rate = frame_rate * multiplier;
        rates_.sub_frame_rate = attr_number(tt, "subFrameRate", 1);
        rates_.tick_rate = attr_number(tt, "tickRate", tt.attribute("frameRate") != nullptr ? rates_.frame_rate * rates_.sub_frame_rate : 1);
        const XmlNode *body = tt.child("body");
        if (body == nullptr) return true;  // nothing to show
        if (!timed(*body, 0, kOpen, false, error)) return false;
        std::stable_sort(out_->cues.begin(), out_->cues.end(),
                         [](const SubtitleCue &a, const SubtitleCue &b) { return a.start_us < b.start_us; });
        return true;
    }

 private:
    // The interval of `n` inside a parent that runs [pb, pe), `ref` being where its own clock starts
    // (the parent's begin, or in a seq the previous sibling's end).
    bool interval(const XmlNode &n, int64_t ref, int64_t pe, int64_t *b, int64_t *e, std::string *error) {
        int64_t begin = 0, end = 0, dur = 0;
        const std::string *bs = n.attribute("begin"), *es = n.attribute("end"), *ds = n.attribute("dur");
        if ((bs != nullptr && !parse_ttml_time(*bs, rates_, &begin)) || (es != nullptr && !parse_ttml_time(*es, rates_, &end)) ||
            (ds != nullptr && !parse_ttml_time(*ds, rates_, &dur))) {
            *error = "bad time expression in <" + n.local() + ">";
            return false;
        }
        *b = ref == kOpen ? kOpen : ref + begin;
        *e = pe;
        if (es != nullptr && ref != kOpen) *e = std::min(*e, ref + end);
        if (ds != nullptr && *b != kOpen) *e = std::min(*e, *b + dur);
        return true;
    }

    // body and div: containers of paragraphs.
    bool timed(const XmlNode &n, int64_t ref, int64_t pe, bool preserve, std::string *error) {
        int64_t b = 0, e = 0;
        if (!interval(n, ref, pe, &b, &e, error)) return false;
        preserve = space(n, preserve);
        const bool seq = n.attribute_or("timeContainer", "par") == "seq";
        int64_t cursor = b;
        for (const XmlNode &c : n.children) {
            const std::string name = c.local();
            if (name == "div") {
                if (!timed(c, seq ? cursor : b, e, preserve, error)) return false;
            } else if (name == "p") {
                if (!paragraph(c, seq ? cursor : b, e, preserve, error)) return false;
            } else {
                continue;
            }
            if (seq) {
                int64_t cb = 0, ce = 0;
                if (!interval(c, cursor, e, &cb, &ce, error)) return false;
                cursor = ce;
            }
        }
        return true;
    }

    static bool space(const XmlNode &n, bool inherited) {
        const std::string *v = n.attribute("xml:space");
        if (v == nullptr) v = n.attribute("space");
        return v != nullptr ? *v == "preserve" : inherited;
    }

    // Text runs and line breaks of a p or span, each with the interval it shows in.
    bool fragments(const XmlNode &n, int64_t b, int64_t e, bool preserve, std::vector<Fragment> *out, std::string *error) {
        const bool seq = n.attribute_or("timeContainer", "par") == "seq";
        int64_t cursor = b;
        for (const XmlNode &c : n.children) {
            const std::string name = c.local();
            if (name == "#text") {
                Fragment f;
                f.begin = b;
                f.end = e;
                f.text = preserve ? c.text : collapse(c.text);
                out->push_back(std::move(f));
            } else if (name == "br") {
                Fragment f;
                f.begin = b;
                f.end = e;
                f.line_break = true;
                out->push_back(std::move(f));
            } else if (name == "span") {
                int64_t cb = 0, ce = 0;
                if (!interval(c, seq ? cursor : b, e, &cb, &ce, error)) return false;
                if (seq) cursor = ce;
                if (cb >= ce) continue;
                if (!fragments(c, cb, ce, space(c, preserve), out, error)) return false;
            }
            // metadata, set, animation, images: nothing to draw
        }
        return true;
    }

    bool paragraph(const XmlNode &p, int64_t ref, int64_t pe, bool preserve, std::string *error) {
        int64_t b = 0, e = 0;
        if (!interval(p, ref, pe, &b, &e, error)) return false;
        if (b == kOpen || b >= e) return true;
        std::vector<Fragment> frags;
        if (!fragments(p, b, e, space(p, preserve), &frags, error)) return false;
        std::vector<int64_t> cuts{b, e};
        for (const Fragment &f : frags) {
            cuts.push_back(std::clamp(f.begin, b, e));
            cuts.push_back(std::clamp(f.end, b, e));
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        const size_t first = out_->cues.size();
        for (size_t i = 0; i + 1 < cuts.size(); ++i) {
            const int64_t t0 = cuts[i], t1 = cuts[i + 1];
            std::string text;
            for (const Fragment &f : frags) {
                if (f.begin > t0 || f.end < t1) continue;
                if (f.line_break) {
                    text += '\n';
                    continue;
                }
                size_t from = 0;
                if (!preserve && !f.text.empty() && f.text[0] == ' ' && (text.empty() || text.back() == ' ' || text.back() == '\n')) {
                    from = 1;  // one space between runs, none at a line start
                }
                text.append(f.text, from, std::string::npos);
            }
            text = tidy(text);
            if (text.empty()) continue;
            if (out_->cues.size() > first && out_->cues.back().text == text && out_->cues.back().end_us == t0) {
                out_->cues.back().end_us = t1 == kOpen ? kNoTime : t1;
                continue;
            }
            SubtitleCue c;
            c.start_us = t0;
            c.end_us = t1 == kOpen ? kNoTime : t1;
            c.text = std::move(text);
            out_->cues.push_back(std::move(c));
        }
        return true;
    }

    // Lines trimmed, blank ones dropped.
    static std::string tidy(const std::string &s) {
        std::string out;
        size_t start = 0;
        while (start <= s.size()) {
            size_t nl = s.find('\n', start);
            if (nl == std::string::npos) nl = s.size();
            size_t a = start, z = nl;
            while (a < z && is_space(s[a])) ++a;
            while (z > a && is_space(s[z - 1])) --z;
            if (z > a) {
                if (!out.empty()) out += '\n';
                out.append(s, a, z - a);
            }
            start = nl + 1;
        }
        return out;
    }

    TtmlFile *out_;
    TtmlRates rates_;
};

}  // namespace

bool parse_ttml_time(const std::string &s, const TtmlRates &rates, int64_t *us) {
    auto to_us = [us](long double seconds) {
        if (!(seconds >= 0) || seconds > 1e9L) return false;
        *us = static_cast<int64_t>(std::llround(seconds * 1000000.0L));
        return true;
    };
    std::string t = s;
    while (!t.empty() && is_space(t.back())) t.pop_back();
    while (!t.empty() && is_space(t.front())) t.erase(t.begin());
    if (t.find(':') != std::string::npos) {
        // hours:minutes:seconds[.fraction] or hours:minutes:seconds:frames[.sub-frames]
        std::vector<std::string> parts;
        size_t from = 0;
        for (size_t c; (c = t.find(':', from)) != std::string::npos; from = c + 1) parts.push_back(t.substr(from, c - from));
        parts.push_back(t.substr(from));
        if (parts.size() != 3 && parts.size() != 4) return false;
        if (!digits(parts[0], 0, parts[0].size()) || parts[1].size() != 2 || !digits(parts[1], 0, 2)) return false;
        const long double h = std::strtold(parts[0].c_str(), nullptr), m = std::strtold(parts[1].c_str(), nullptr);
        double sec = 0;
        if (parts[2].size() < 2 || !digits(parts[2], 0, 2) || !decimal(parts[2], &sec) || m >= 60 || sec >= 61) return false;
        long double total = h * 3600 + m * 60 + sec;
        if (parts.size() == 4) {
            if (parts[2].size() != 2) return false;  // no fraction before frames
            const std::string &fr = parts[3];
            const size_t dot = fr.find('.');
            if (!digits(fr, 0, dot == std::string::npos ? fr.size() : dot)) return false;
            const long double frames = std::strtold(fr.substr(0, dot).c_str(), nullptr);
            long double sub = 0;
            if (dot != std::string::npos) {
                if (!digits(fr, dot + 1, fr.size())) return false;
                sub = std::strtold(fr.c_str() + dot + 1, nullptr);
            }
            if (rates.frame_rate <= 0) return false;
            total += (frames + sub / std::max(1.0, rates.sub_frame_rate)) / rates.frame_rate;
        }
        return to_us(total);
    }
    // Offset time: a number and a metric.
    size_t n = 0;
    while (n < t.size() && ((t[n] >= '0' && t[n] <= '9') || t[n] == '.')) ++n;
    double v = 0;
    if (n == 0 || !decimal(t.substr(0, n), &v)) return false;
    const std::string metric = t.substr(n);
    if (metric == "h") return to_us(static_cast<long double>(v) * 3600);
    if (metric == "m") return to_us(static_cast<long double>(v) * 60);
    if (metric == "s") return to_us(v);
    if (metric == "ms") return to_us(static_cast<long double>(v) / 1000);
    if (metric == "f") return rates.frame_rate > 0 && to_us(static_cast<long double>(v) / rates.frame_rate);
    if (metric == "t") return rates.tick_rate > 0 && to_us(static_cast<long double>(v) / rates.tick_rate);
    return false;
}

bool parse_ttml(const std::string &doc, TtmlFile *out, std::string *error) {
    out->cues.clear();
    XmlNode root;
    XmlLimits limits;
    limits.text_children = true;
    if (!parse_xml(doc, &root, error, limits)) return false;
    if (root.local() != "tt") {
        *error = "not TTML (root <" + root.name + ">)";
        return false;
    }
    return Reader(out).run(root, error);
}

bool parse_stpp_fragment(const uint8_t *data, size_t size, uint32_t timescale, std::vector<SubtitleCue> *out,
                         std::string *error) {
    std::vector<Mp4TextSample> samples;
    if (!mp4_text_samples(data, size, timescale, &samples, error)) return false;
    for (const Mp4TextSample &s : samples) {
        std::string doc(reinterpret_cast<const char *>(s.data), s.size);
        // A sample can carry images after the document (sub-samples) or padding: keep the document.
        const size_t close = doc.rfind("tt>");
        if (close != std::string::npos) doc.resize(close + 3);
        TtmlFile f;
        std::string why;
        if (doc.find_first_not_of(" \t\r\n", 0) == std::string::npos) continue;  // an empty sample: a gap
        if (!parse_ttml(doc, &f, &why)) {
            *error = "TTML sample at " + std::to_string(s.start_us / 1000) + " ms: " + why;
            return false;
        }
        bool before = !f.cues.empty() && s.start_us > 0;
        for (const SubtitleCue &c : f.cues) before = before && c.end_us != kNoTime && c.end_us <= s.start_us;
        for (SubtitleCue &c : f.cues) {
            if (before) {
                c.start_us += s.start_us;
                c.end_us += s.start_us;
            } else if (c.end_us == kNoTime) {
                c.end_us = s.end_us;  // the sample is as long as a document lasts
            }
            out->push_back(std::move(c));
        }
    }
    return true;
}

}  // namespace wnk::media
