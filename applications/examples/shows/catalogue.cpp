// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace shows {

using nlohmann::json;
using wnk::net::NetError;

namespace {

// Typed reads that never throw: the API is not ours, so a field of the wrong type is "absent".
std::string str(const json &o, const char *key) {
    auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int64_t integer(const json &o, const char *key) {
    auto it = o.find(key);
    if (it == o.end()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
    return 0;
}

double number(const json &o, const char *key) {
    auto it = o.find(key);
    return it != o.end() && it->is_number() ? it->get<double>() : 0.0;
}

const json *object(const json &o, const char *key) {
    auto it = o.find(key);
    return it != o.end() && it->is_object() ? &*it : nullptr;
}

int year_of(const std::string &date) {
    if (date.size() < 4) return 0;
    int y = 0;
    for (int i = 0; i < 4; ++i) {
        if (date[i] < '0' || date[i] > '9') return 0;
        y = y * 10 + (date[i] - '0');
    }
    return y;
}

bool parse_show(const json &o, Show *s) {
    if (!o.is_object()) return false;
    s->id = static_cast<int>(integer(o, "id"));
    s->name = str(o, "name");
    if (s->id <= 0 || s->name.empty()) return false;
    if (const json *img = object(o, "image")) {
        s->image = str(*img, "medium");
        // http(s) only: a file:// URL from the network would point the image loader at local files.
        if (s->image.rfind("https://", 0) != 0 && s->image.rfind("http://", 0) != 0) s->image.clear();
    }
    if (const json *r = object(o, "rating")) {
        const double v = number(*r, "average");
        s->rating = v > 0 && v <= 10 ? static_cast<float>(v) : 0.0f;
    }
    s->weight = static_cast<int>(std::clamp<int64_t>(integer(o, "weight"), 0, 100));
    s->year = year_of(str(o, "premiered"));
    s->updated = integer(o, "updated");
    return true;
}

void append_utf8(std::string &out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Decodes the entity starting at text[i] == '&'. Returns the characters consumed (0: not an entity).
size_t decode_entity(std::string_view text, size_t i, std::string &out) {
    const size_t semi = text.find(';', i);
    if (semi == std::string_view::npos || semi - i > 10) return 0;
    const std::string_view name = text.substr(i + 1, semi - i - 1);
    if (!name.empty() && name[0] == '#') {
        const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        const std::string digits(name.substr(hex ? 2 : 1));
        if (digits.empty()) return 0;
        char *end = nullptr;
        const unsigned long cp = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
        if (end == nullptr || *end != '\0') return 0;
        append_utf8(out, cp > 0x10FFFF ? 0xFFFD : static_cast<uint32_t>(cp));
        return semi - i + 1;
    }
    static const struct {
        const char *name;
        uint32_t cp;
    } kNamed[] = {{"amp", '&'},     {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
                  {"nbsp", ' '},    {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026}, {"rsquo", 0x2019},
                  {"lsquo", 0x2018}, {"rdquo", 0x201D}, {"ldquo", 0x201C}, {"eacute", 0xE9}};
    for (const auto &e : kNamed) {
        if (name == e.name) {
            append_utf8(out, e.cp);
            return semi - i + 1;
        }
    }
    return 0;
}

std::string tag_name(std::string_view tag) {
    size_t p = 0;
    if (p < tag.size() && tag[p] == '/') ++p;
    std::string name;
    while (p < tag.size() && std::isalnum(static_cast<unsigned char>(tag[p])))
        name += static_cast<char>(std::tolower(static_cast<unsigned char>(tag[p++])));
    return name;
}

std::vector<Show> top_by(const std::vector<Show> &all, size_t n, bool (*before)(const Show &, const Show &),
                         bool (*keep)(const Show &)) {
    std::vector<Show> v;
    for (const Show &s : all)
        if (keep(s)) v.push_back(s);
    std::stable_sort(v.begin(), v.end(), before);
    if (v.size() > n) v.resize(n);
    return v;
}

}  // namespace

bool valid_base(const std::string &base) {
    const bool scheme = base.rfind("https://", 0) == 0 || base.rfind("http://", 0) == 0;
    return scheme && base.size() > 8 && base.back() != '/' &&
           base.find_first_of(" \r\n\t?#") == std::string::npos;
}

std::string page_url(const std::string &base, int page) { return base + "/shows?page=" + std::to_string(page); }

std::string details_url(const std::string &base, int id) {
    // Brackets are percent-encoded: curl rejects raw [] unless globbing is off, and so do proxies.
    return base + "/shows/" + std::to_string(id) + "?embed%5B%5D=seasons&embed%5B%5D=cast";
}

bool parse_show_list(std::string_view text, std::vector<Show> *out, std::string *why) {
    const json doc = json::parse(text.begin(), text.end(), nullptr, false);
    if (doc.is_discarded()) {
        *why = "not JSON";
        return false;
    }
    if (!doc.is_array()) {
        *why = "expected a JSON array of shows";
        return false;
    }
    out->clear();
    out->reserve(doc.size());
    for (const json &o : doc) {
        Show s;
        if (parse_show(o, &s)) out->push_back(std::move(s));
    }
    return true;
}

bool parse_details(std::string_view text, Details *out, std::string *why) {
    const json doc = json::parse(text.begin(), text.end(), nullptr, false);
    if (doc.is_discarded()) {
        *why = "not JSON";
        return false;
    }
    if (!parse_show(doc, &out->show)) {
        *why = "expected a show object with an id and a name";
        return false;
    }
    out->summary = strip_html(str(doc, "summary"));
    out->status = str(doc, "status");
    out->language = str(doc, "language");
    out->premiered = str(doc, "premiered");
    out->ended = str(doc, "ended");
    out->runtime = static_cast<int>(std::clamp<int64_t>(integer(doc, "runtime"), 0, 10000));
    if (out->runtime == 0) out->runtime = static_cast<int>(std::clamp<int64_t>(integer(doc, "averageRuntime"), 0, 10000));
    if (const json *n = object(doc, "network")) out->network = str(*n, "name");
    if (out->network.empty())
        if (const json *w = object(doc, "webChannel")) out->network = str(*w, "name");
    if (auto g = doc.find("genres"); g != doc.end() && g->is_array())
        for (const json &v : *g)
            if (v.is_string()) out->genres.push_back(v.get<std::string>());
    if (const json *emb = object(doc, "_embedded")) {
        if (auto s = emb->find("seasons"); s != emb->end() && s->is_array()) {
            for (const json &v : *s) {
                if (!v.is_object()) continue;
                Season season;
                season.number = static_cast<int>(integer(v, "number"));
                season.episodes = static_cast<int>(std::max<int64_t>(0, integer(v, "episodeOrder")));
                season.premiere = str(v, "premiereDate");
                if (season.number > 0) out->seasons.push_back(std::move(season));
            }
        }
        if (auto c = emb->find("cast"); c != emb->end() && c->is_array()) {
            for (const json &v : *c) {
                if (!v.is_object()) continue;
                CastMember m;
                if (const json *p = object(v, "person")) m.person = str(*p, "name");
                if (const json *ch = object(v, "character")) m.character = str(*ch, "name");
                if (!m.person.empty()) out->cast.push_back(std::move(m));
            }
        }
    }
    return true;
}

std::string strip_html(std::string_view html) {
    std::string out;
    out.reserve(html.size());
    bool space = false;  // a collapsed run of whitespace is pending
    auto emit_break = [&out, &space] {
        space = false;
        while (!out.empty() && out.back() == ' ') out.pop_back();
        if (out.empty()) return;
        if (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') return;
        out += out.back() == '\n' ? "\n" : "\n\n";
    };
    for (size_t i = 0; i < html.size();) {
        const char c = html[i];
        if (c == '<') {
            const size_t close = html.find('>', i);
            if (close == std::string_view::npos) break;  // truncated tag: drop the rest
            const std::string name = tag_name(html.substr(i + 1, close - i - 1));
            if (name == "p" || name == "br" || name == "div" || name == "li") emit_break();
            i = close + 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = true;
            ++i;
            continue;
        }
        if (space && !out.empty() && out.back() != '\n') out += ' ';
        space = false;
        if (c == '&') {
            if (const size_t n = decode_entity(html, i, out)) {
                i += n;
                continue;
            }
        }
        out += c;
        ++i;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out;
}

std::string describe_failure(NetError error, long status) {
    switch (error) {
    case NetError::None:
        if (status == 404) return "The catalogue does not have this entry.";
        if (status == 429) return "The catalogue is busy. Try again in a few seconds.";
        if (status >= 500) return "The catalogue server has a problem (HTTP " + std::to_string(status) + ").";
        return "The catalogue refused the request (HTTP " + std::to_string(status) + ").";
    case NetError::Dns:
    case NetError::Connect: return "Cannot reach the catalogue. Check the TV's network connection.";
    case NetError::Timeout: return "The catalogue took too long to answer.";
    case NetError::Tls: return "The secure connection failed. Check the TV's date and time.";
    case NetError::TooLarge: return "The catalogue sent more data than the app accepts.";
    case NetError::Unsupported: return "This TV has no HTTP library the app can use.";
    case NetError::Cancelled: return "The request was cancelled.";
    case NetError::InvalidRequest: return "The catalogue address is not valid.";
    case NetError::Protocol:
    case NetError::Io: return "The connection to the catalogue broke off.";
    }
    return "The request failed.";
}

bool is_transient(NetError error, long status) {
    switch (error) {
    case NetError::None: return status == 429 || status >= 500;
    case NetError::Dns:
    case NetError::Connect:
    case NetError::Timeout:
    case NetError::Protocol:
    case NetError::Io: return true;
    default: return false;
    }
}

std::string encode_shows(const std::vector<Show> &shows, int64_t saved_at, size_t max_bytes, size_t *kept) {
    json arr = json::array();
    for (const Show &s : shows) {
        json o = {{"i", s.id}, {"n", s.name}};
        if (!s.image.empty()) o["p"] = s.image;
        if (s.rating > 0) o["r"] = std::round(s.rating * 10) / 10;
        if (s.weight > 0) o["w"] = s.weight;
        if (s.year > 0) o["y"] = s.year;
        if (s.updated > 0) o["u"] = s.updated;
        arr.push_back(std::move(o));
    }
    // Version first, so a future format can be told apart without guessing from the fields.
    for (;;) {
        std::string text = json{{"v", 1}, {"saved", saved_at}, {"shows", arr}}.dump();
        if (text.size() <= max_bytes || arr.empty()) {
            if (kept != nullptr) *kept = arr.size();
            return text;
        }
        // Drop about the share of entries that overflows (at least one) and try again.
        const size_t over = text.size() - max_bytes;
        const size_t per = std::max<size_t>(1, text.size() / arr.size());
        const size_t drop = std::min(arr.size(), over / per + 1);
        arr.erase(arr.end() - static_cast<std::ptrdiff_t>(drop), arr.end());
    }
}

bool decode_shows(std::string_view text, std::vector<Show> *out, int64_t *saved_at) {
    const json doc = json::parse(text.begin(), text.end(), nullptr, false);
    if (!doc.is_object() || integer(doc, "v") != 1) return false;
    auto it = doc.find("shows");
    if (it == doc.end() || !it->is_array()) return false;
    out->clear();
    for (const json &o : *it) {
        if (!o.is_object()) continue;
        Show s;
        s.id = static_cast<int>(integer(o, "i"));
        s.name = str(o, "n");
        if (s.id <= 0 || s.name.empty()) continue;
        s.image = str(o, "p");
        s.rating = static_cast<float>(number(o, "r"));
        s.weight = static_cast<int>(integer(o, "w"));
        s.year = static_cast<int>(integer(o, "y"));
        s.updated = integer(o, "u");
        out->push_back(std::move(s));
    }
    if (saved_at != nullptr) *saved_at = integer(doc, "saved");
    return true;
}

void set_favourite(std::vector<Show> *list, const Show &show, bool on, size_t max) {
    list->erase(std::remove_if(list->begin(), list->end(), [&show](const Show &s) { return s.id == show.id; }),
                list->end());
    if (on) list->insert(list->begin(), show);
    if (list->size() > max) list->resize(max);
}

bool contains(const std::vector<Show> &list, int id) {
    return std::any_of(list.begin(), list.end(), [id](const Show &s) { return s.id == id; });
}

std::vector<Show> top_rated(const std::vector<Show> &all, size_t n) {
    return top_by(all, n, [](const Show &a, const Show &b) { return a.rating > b.rating; },
                  [](const Show &s) { return s.rating > 0; });
}

std::vector<Show> most_popular(const std::vector<Show> &all, size_t n) {
    return top_by(all, n, [](const Show &a, const Show &b) { return a.weight > b.weight; },
                  [](const Show &) { return true; });
}

std::vector<Show> recently_updated(const std::vector<Show> &all, size_t n) {
    return top_by(all, n, [](const Show &a, const Show &b) { return a.updated > b.updated; },
                  [](const Show &s) { return s.updated > 0; });
}

std::string age_text(int64_t seconds) {
    if (seconds < 0) return "";
    if (seconds < 60) return "just now";
    if (seconds < 3600) return std::to_string(seconds / 60) + " min ago";
    if (seconds < 86400) return std::to_string(seconds / 3600) + " h ago";
    const int64_t days = seconds / 86400;
    return days == 1 ? "1 day ago" : std::to_string(days) + " days ago";
}

std::string card_subtitle(const Show &s) {
    char rating[16] = "";
    if (s.rating > 0) std::snprintf(rating, sizeof rating, "%.1f", static_cast<double>(s.rating));
    if (s.year > 0 && rating[0] != '\0') return std::to_string(s.year) + " · " + rating;
    if (s.year > 0) return std::to_string(s.year);
    return rating;
}

}  // namespace shows
