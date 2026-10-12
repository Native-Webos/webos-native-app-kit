// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/i18n/i18n.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace wnk::i18n {
namespace {

bool all_alpha(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; });
}
bool all_digit(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}
std::string lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
std::string upper(std::string_view s) {
    std::string out(s);
    for (char &c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool one_of(std::string_view lang, std::initializer_list<const char *> list) {
    return std::any_of(list.begin(), list.end(), [lang](const char *l) { return lang == l; });
}

}  // namespace

// ---- Locale ---------------------------------------------------------------------------------------

Locale Locale::parse(std::string_view tag) {
    const size_t cut = tag.find_first_of(".@");
    if (cut != std::string_view::npos) tag = tag.substr(0, cut);
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= tag.size()) {
        const size_t sep = tag.find_first_of("-_", start);
        parts.push_back(tag.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start));
        if (sep == std::string_view::npos) break;
        start = sep + 1;
    }
    Locale l;
    if (parts.empty() || !all_alpha(parts[0]) || parts[0].size() < 2 || parts[0].size() > 3) {
        l.language = "en";
        return l;
    }
    l.language = lower(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
        const std::string_view p = parts[i];
        if (l.script.empty() && l.region.empty() && p.size() == 4 && all_alpha(p)) {
            l.script = lower(p);
            l.script[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(l.script[0])));
        } else if (l.region.empty() && ((p.size() == 2 && all_alpha(p)) || (p.size() == 3 && all_digit(p)))) {
            l.region = upper(p);
        }
    }
    return l;
}

std::string Locale::tag() const {
    std::string t = language;
    if (!script.empty()) t += "-" + script;
    if (!region.empty()) t += "-" + region;
    return t;
}

bool Locale::traditional_chinese() const {
    if (language != "zh") return false;
    if (!script.empty()) return script == "Hant";
    return region == "TW" || region == "HK" || region == "MO";
}

std::vector<std::string> fallback_chain(const Locale &l) {
    std::vector<std::string> out;
    auto add = [&out](std::string t) {
        if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(std::move(t));
    };
    std::string script = l.script;
    if (script.empty() && l.language == "zh") script = l.traditional_chinese() ? "Hant" : "Hans";
    if (!script.empty() && !l.region.empty()) add(l.language + "-" + script + "-" + l.region);
    if (!l.region.empty()) add(l.language + "-" + l.region);
    if (!script.empty()) add(l.language + "-" + script);
    add(l.language);
    // A Chinese-language UI without a region-specific file: the right script beats nothing at all.
    return out;
}

std::vector<std::string> font_preferences(const Locale &l) {
    if (l.language == "ja") return {"JP", "Japanese"};
    if (l.language == "ko") return {"KR", "Korean"};
    if (l.language == "zh") {
        if (l.region == "HK" || l.region == "MO") return {"HK", "TC", "TW"};
        return l.traditional_chinese() ? std::vector<std::string>{"TC", "TW", "HK"} : std::vector<std::string>{"SC", "CN"};
    }
    return {};
}

// ---- plurals --------------------------------------------------------------------------------------

const char *plural_name(Plural p) {
    switch (p) {
        case Plural::Zero: return "zero";
        case Plural::One: return "one";
        case Plural::Two: return "two";
        case Plural::Few: return "few";
        case Plural::Many: return "many";
        case Plural::Other: return "other";
    }
    return "other";
}

// CLDR 44 cardinal rules, integer operands only (v = 0).
Plural plural_category(std::string_view language, int64_t count) {
    const std::string lang = lower(language);
    const uint64_t n = count < 0 ? static_cast<uint64_t>(-(count + 1)) + 1 : static_cast<uint64_t>(count);
    const uint64_t n10 = n % 10, n100 = n % 100;
    if (one_of(lang, {"ja", "zh", "ko", "th", "vi", "id", "ms", "lo", "km", "my", "yue", "jv", "su"})) return Plural::Other;
    if (one_of(lang, {"fr", "pt", "hi", "bn", "fa", "gu", "kn", "mr", "zu", "am", "hy"})) {
        // fr, pt (Brazilian rule) and Indic languages: 0 and 1 are "one".
        return n <= 1 ? Plural::One : Plural::Other;
    }
    if (one_of(lang, {"ru", "uk", "be"})) {
        if (n10 == 1 && n100 != 11) return Plural::One;
        if (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14)) return Plural::Few;
        return Plural::Many;
    }
    if (lang == "pl") {
        if (n == 1) return Plural::One;
        if (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14)) return Plural::Few;
        return Plural::Many;
    }
    if (one_of(lang, {"cs", "sk"})) {
        if (n == 1) return Plural::One;
        if (n >= 2 && n <= 4) return Plural::Few;
        return Plural::Other;
    }
    if (one_of(lang, {"hr", "sr", "bs"})) {
        if (n10 == 1 && n100 != 11) return Plural::One;
        if (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14)) return Plural::Few;
        return Plural::Other;
    }
    if (lang == "sl") {
        if (n100 == 1) return Plural::One;
        if (n100 == 2) return Plural::Two;
        if (n100 == 3 || n100 == 4) return Plural::Few;
        return Plural::Other;
    }
    if (lang == "lt") {
        if (n10 == 1 && (n100 < 11 || n100 > 19)) return Plural::One;
        if (n10 >= 2 && (n100 < 11 || n100 > 19)) return Plural::Few;
        return Plural::Other;
    }
    if (lang == "lv") {
        if (n10 == 0 || (n100 >= 11 && n100 <= 19)) return Plural::Zero;
        if (n10 == 1 && n100 != 11) return Plural::One;
        return Plural::Other;
    }
    if (lang == "ro") {
        if (n == 1) return Plural::One;
        if (n == 0 || (n100 >= 2 && n100 <= 19)) return Plural::Few;
        return Plural::Other;
    }
    if (lang == "ar") {
        if (n == 0) return Plural::Zero;
        if (n == 1) return Plural::One;
        if (n == 2) return Plural::Two;
        if (n100 >= 3 && n100 <= 10) return Plural::Few;
        if (n100 >= 11) return Plural::Many;
        return Plural::Other;
    }
    if (lang == "he") {
        if (n == 1) return Plural::One;
        if (n == 2) return Plural::Two;
        return Plural::Other;
    }
    if (lang == "ga") {
        if (n == 1) return Plural::One;
        if (n == 2) return Plural::Two;
        if (n >= 3 && n <= 6) return Plural::Few;
        if (n >= 7 && n <= 10) return Plural::Many;
        return Plural::Other;
    }
    if (lang == "cy") {
        if (n == 0) return Plural::Zero;
        if (n == 1) return Plural::One;
        if (n == 2) return Plural::Two;
        if (n == 3) return Plural::Few;
        if (n == 6) return Plural::Many;
        return Plural::Other;
    }
    return n == 1 ? Plural::One : Plural::Other;  // en, de, nl, sv, da, nb, fi, it, es, el, hu, tr, ...
}

// ---- numbers --------------------------------------------------------------------------------------

namespace {

struct Separators {
    std::string group, decimal;
    int min_grouping = 1;  // digits before the first group separator is used: es and pl write 1234
};

Separators separators(const Locale &l) {
    const std::string &lang = l.language;
    if ((lang == "de" || lang == "it") && (l.region == "CH" || l.region == "LI")) return {"\xE2\x80\x99", "."};
    if (lang == "fr") return {"\xE2\x80\xAF", ","};  // narrow no-break space
    if (lang == "es" || lang == "pl") return {lang == "es" ? "." : "\xC2\xA0", ",", 2};
    if (lang == "pt" && l.region == "PT") return {"\xC2\xA0", ","};
    if (one_of(lang, {"de", "nl", "id", "it", "pt", "da", "tr", "el", "ro", "hr", "sr", "sl", "vi", "ca", "gl", "eu"})) return {".", ","};
    if (one_of(lang, {"ru", "uk", "be", "cs", "sk", "fi", "sv", "nb", "no", "nn", "hu", "bg", "lt", "lv", "et", "kk",
                      "ka", "az"})) {
        return {"\xC2\xA0", ","};  // no-break space
    }
    return {",", "."};
}

std::string group_digits(const std::string &digits, const Separators &sep) {
    if (digits.size() <= 3 || static_cast<int>(digits.size()) < 4 + sep.min_grouping - 1) return digits;
    std::string out;
    const size_t first = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    out += digits.substr(0, first);
    for (size_t i = first; i < digits.size(); i += 3) out += sep.group + digits.substr(i, 3);
    return out;
}

}  // namespace

std::string format_number(int64_t n, const Locale &locale) {
    const Separators sep = separators(locale);
    const uint64_t magnitude = n < 0 ? static_cast<uint64_t>(-(n + 1)) + 1 : static_cast<uint64_t>(n);
    return (n < 0 ? "-" : "") + group_digits(std::to_string(magnitude), sep);
}

std::string format_number(double value, int decimals, const Locale &locale) {
    if (!std::isfinite(value)) return std::isnan(value) ? "NaN" : (value < 0 ? "-\xE2\x88\x9E" : "\xE2\x88\x9E");
    decimals = std::clamp(decimals, 0, 9);
    const Separators sep = separators(locale);
    double scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    const bool negative = value < 0;
    const double rounded = std::round(std::fabs(value) * scale);
    if (rounded > 9.0e18) return std::to_string(value);
    const auto units = static_cast<uint64_t>(rounded);
    const auto whole = static_cast<uint64_t>(units / static_cast<uint64_t>(scale));
    std::string out = (negative && units != 0 ? "-" : "") + group_digits(std::to_string(whole), sep);
    if (decimals > 0) {
        std::string frac = std::to_string(units % static_cast<uint64_t>(scale));
        frac.insert(0, static_cast<size_t>(decimals) - frac.size(), '0');
        out += sep.decimal + frac;
    }
    return out;
}

bool parse_locale_info(std::string_view reply_json, Locale *ui, Locale *formats) {
    const auto j = nlohmann::json::parse(reply_json, nullptr, false);
    if (!j.is_object() || !j.contains("settings") || !j["settings"].is_object()) return false;
    const auto &s = j["settings"];
    if (!s.contains("localeInfo") || !s["localeInfo"].is_object()) return false;
    const auto &info = s["localeInfo"];
    if (!info.contains("locales") || !info["locales"].is_object()) return false;
    const auto &loc = info["locales"];
    if (!loc.contains("UI") || !loc["UI"].is_string()) return false;
    if (ui != nullptr) *ui = Locale::parse(loc["UI"].get<std::string>());
    if (formats != nullptr) {
        *formats = loc.contains("FMT") && loc["FMT"].is_string() ? Locale::parse(loc["FMT"].get<std::string>())
                                                                 : Locale::parse(loc["UI"].get<std::string>());
    }
    return true;
}

// ---- catalog --------------------------------------------------------------------------------------

std::string substitute(std::string_view text, const Args &args) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const size_t open = text.find('{', i);
        if (open == std::string_view::npos) {
            out.append(text.substr(i));
            break;
        }
        const size_t close = text.find('}', open + 1);
        if (close == std::string_view::npos) {
            out.append(text.substr(i));
            break;
        }
        out.append(text.substr(i, open - i));
        const std::string_view name = text.substr(open + 1, close - open - 1);
        auto it = std::find_if(args.begin(), args.end(), [name](const auto &a) { return a.first == name; });
        if (it != args.end()) {
            out += it->second;
        } else {
            out.append(text.substr(open, close - open + 1));
        }
        i = close + 1;
    }
    return out;
}

bool Catalog::add_json(std::string_view json, std::string *error) {
    const auto j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object()) {
        if (error != nullptr) *error = "not a JSON object";
        return false;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it->is_string()) {
            singles_.emplace(it.key(), it->get<std::string>());
        } else if (it->is_object()) {
            std::map<Plural, std::string> forms;
            for (auto f = it->begin(); f != it->end(); ++f) {
                if (!f->is_string()) continue;
                for (Plural p : {Plural::Zero, Plural::One, Plural::Two, Plural::Few, Plural::Many, Plural::Other}) {
                    if (f.key() == plural_name(p)) forms[p] = f->get<std::string>();
                }
            }
            if (!forms.empty()) plurals_.emplace(it.key(), std::move(forms));
        } else if (error != nullptr) {
            *error = "entry \"" + it.key() + "\" is neither a string nor an object of plural forms";
        }
    }
    return true;
}

int Catalog::load_dir(const std::string &dir, std::string *error) {
    int read = 0;
    for (const auto &tag : fallback_chain(locale_)) {
        std::ifstream in(dir + "/" + tag + ".json", std::ios::binary);
        if (!in) continue;
        std::stringstream buf;
        buf << in.rdbuf();
        std::string err;
        if (!add_json(buf.str(), &err)) {
            if (error != nullptr) *error = tag + ".json: " + err;
            continue;
        }
        ++read;  // a bad entry is skipped; the others are used
        if (!err.empty() && error != nullptr) *error = tag + ".json: " + err;
    }
    return read;
}

std::string Catalog::tr(std::string_view source, const Args &args) const {
    auto it = singles_.find(source);
    return substitute(it != singles_.end() ? std::string_view(it->second) : source, args);
}

std::string Catalog::trn(std::string_view one, std::string_view other, int64_t n, const Args &args) const {
    Args all = args;
    all.emplace_back("n", format_number(n, locale_));
    auto it = plurals_.find(one);
    if (it != plurals_.end()) {
        const auto &forms = it->second;
        auto f = forms.find(plural_category(locale_.language, n));
        if (f == forms.end()) f = forms.find(Plural::Other);
        if (f != forms.end()) return substitute(f->second, all);
    }
    return substitute(n == 1 ? one : other, all);
}

}  // namespace wnk::i18n
