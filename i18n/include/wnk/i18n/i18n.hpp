// SPDX-License-Identifier: GPL-3.0-or-later
//
// Localization: the locale, string catalogs with plural forms, and number formatting.
//
// Catalogs are gettext-style: the English source text is the key, so code reads
// tr("Search by name") and English needs no file. A translation is <dir>/<tag>.json:
//
//   { "Search by name": "Rechercher par nom",
//     "{n} show": { "one": "{n} série", "other": "{n} séries" } }
//
// Files are read for the locale's fallback chain (zh-Hant-TW → zh-Hant → zh), the most specific first;
// a key missing from all of them shows the source text. Plural forms use CLDR's cardinal categories
// for integer counts. {name} placeholders are replaced from the arguments; trn() fills {n} with the
// count formatted for the locale.
//
// Not covered: right-to-left layout, date and time formatting, and plural rules for fractions.
// SDL-free; the app shell finds the TV's locale (com.webos.settingsservice localeInfo) and loads the
// app's catalogs (AppConfig::translations_dir).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wnk::i18n {

struct Locale {
    std::string language;  // lower case, "en"
    std::string script;    // title case, "Hant", or empty
    std::string region;    // upper case, "US", or empty

    // "en-US", "en_US.UTF-8", "zh-Hant-TW", "zh_TW", "C": underscores, case and encodings are tolerated.
    // An unusable tag gives "en".
    static Locale parse(std::string_view tag);
    std::string tag() const;  // "zh-Hant-TW"
    bool operator==(const Locale &o) const { return language == o.language && script == o.script && region == o.region; }
    // Chinese written in traditional characters (zh-Hant, or zh-TW / zh-HK / zh-MO without a script).
    bool traditional_chinese() const;
};

// Tags to look up, most specific first: zh-Hant-TW → {"zh-Hant-TW", "zh-Hant", "zh-TW", "zh"}.
std::vector<std::string> fallback_chain(const Locale &locale);

// Font file-name tags that pick the right forms of Han characters for the locale ("JP" for Japanese,
// "KR" for Korean, "SC"/"TC" for Chinese), for render::FontFinder. Empty for other languages.
std::vector<std::string> font_preferences(const Locale &locale);

enum class Plural : uint8_t { Zero, One, Two, Few, Many, Other };
const char *plural_name(Plural p);
// CLDR cardinal category of an integer count in `language`. Languages without a rule here use
// English's (one for 1, other otherwise).
Plural plural_category(std::string_view language, int64_t n);

// Digits with the locale's grouping and decimal separators: 1234567.5 → "1,234,567.5" (en),
// "1.234.567,5" (de), "1 234 567,5" (fr, with U+202F).
std::string format_number(int64_t n, const Locale &locale);
std::string format_number(double value, int decimals, const Locale &locale);

// The UI and formatting locales from a com.webos.settingsservice/getSystemSettings {"keys":["localeInfo"]}
// reply, or false if the reply has none.
bool parse_locale_info(std::string_view reply_json, Locale *ui, Locale *formats);

using Args = std::vector<std::pair<std::string, std::string>>;

// Replaces {name} with the matching argument; unknown placeholders stay as written.
std::string substitute(std::string_view text, const Args &args);

class Catalog {
 public:
    Catalog() = default;
    explicit Catalog(Locale locale) : locale_(std::move(locale)) {}

    // Reads <dir>/<tag>.json for every tag in the locale's fallback chain. Missing files are normal
    // (an English UI has none); a file that is not a JSON object of strings and plural objects is
    // skipped and named in *error. Returns the number of files read.
    int load_dir(const std::string &dir, std::string *error = nullptr);
    // Adds entries from JSON text (tests, or catalogs embedded in the binary). Entries already
    // present (from a more specific file) win.
    bool add_json(std::string_view json, std::string *error = nullptr);

    const Locale &locale() const { return locale_; }
    size_t size() const { return singles_.size() + plurals_.size(); }

    std::string tr(std::string_view source, const Args &args = {}) const;
    // `one` and `other` are the English forms; the count's category picks the translated form.
    std::string trn(std::string_view one, std::string_view other, int64_t n, const Args &args = {}) const;

 private:
    Locale locale_ = Locale::parse("en");
    std::map<std::string, std::string, std::less<>> singles_;
    std::map<std::string, std::map<Plural, std::string>, std::less<>> plurals_;
};

}  // namespace wnk::i18n
