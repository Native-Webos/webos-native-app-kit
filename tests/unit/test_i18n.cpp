// SPDX-License-Identifier: GPL-3.0-or-later
//
// i18n: locale tags and fallback, CLDR plural categories for integers, number formatting, the
// settingsservice localeInfo reply, and catalogs with placeholders and plural forms.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/i18n/i18n.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <unistd.h>

using namespace wnk::i18n;

TEST("locale tags: separators, case, encodings and scripts are normalised") {
    CHECK_EQ(Locale::parse("en-US").tag(), std::string("en-US"));
    CHECK_EQ(Locale::parse("fr_FR.UTF-8").tag(), std::string("fr-FR"));
    CHECK_EQ(Locale::parse("be_BY").tag(), std::string("be-BY"));
    CHECK_EQ(Locale::parse("ZH-hant-tw").tag(), std::string("zh-Hant-TW"));
    CHECK_EQ(Locale::parse("es-419").tag(), std::string("es-419"));
    CHECK_EQ(Locale::parse("de_DE@euro").tag(), std::string("de-DE"));
    CHECK_EQ(Locale::parse("C").tag(), std::string("en"));
    CHECK_EQ(Locale::parse("").tag(), std::string("en"));
    CHECK_EQ(Locale::parse("1234").tag(), std::string("en"));
    CHECK(Locale::parse("zh-TW").traditional_chinese());
    CHECK(Locale::parse("zh-Hant").traditional_chinese());
    CHECK(!Locale::parse("zh-CN").traditional_chinese());
    CHECK(!Locale::parse("zh-Hans-HK").traditional_chinese());
}

TEST("fallback chain: most specific first, Chinese gets its script") {
    auto join = [](const std::vector<std::string> &v) {
        std::string s;
        for (const auto &t : v) s += t + " ";
        return s;
    };
    CHECK_EQ(join(fallback_chain(Locale::parse("en-US"))), std::string("en-US en "));
    CHECK_EQ(join(fallback_chain(Locale::parse("fr"))), std::string("fr "));
    CHECK_EQ(join(fallback_chain(Locale::parse("zh-TW"))), std::string("zh-Hant-TW zh-TW zh-Hant zh "));
    CHECK_EQ(join(fallback_chain(Locale::parse("zh-Hant-HK"))), std::string("zh-Hant-HK zh-HK zh-Hant zh "));
    CHECK_EQ(join(fallback_chain(Locale::parse("zh"))), std::string("zh-Hans zh "));
    auto first_pref = [](const char *tag) {
        const auto prefs = font_preferences(Locale::parse(tag));
        return prefs.empty() ? std::string() : prefs[0];
    };
    CHECK_EQ(first_pref("ja-JP"), std::string("JP"));
    CHECK_EQ(first_pref("zh-TW"), std::string("TC"));
    CHECK_EQ(first_pref("zh-HK"), std::string("HK"));
    CHECK_EQ(first_pref("zh-CN"), std::string("SC"));
    CHECK(font_preferences(Locale::parse("de")).empty());
}

TEST("plural categories follow CLDR for integer counts") {
    CHECK(plural_category("en", 1) == Plural::One);
    CHECK(plural_category("en", 0) == Plural::Other);
    CHECK(plural_category("en", 2) == Plural::Other);
    CHECK(plural_category("fr", 0) == Plural::One);
    CHECK(plural_category("fr", 2) == Plural::Other);
    CHECK(plural_category("ja", 1) == Plural::Other);
    const struct {
        int64_t n;
        Plural ru, pl, cs, ar;
    } cases[] = {
        {1, Plural::One, Plural::One, Plural::One, Plural::One},
        {2, Plural::Few, Plural::Few, Plural::Few, Plural::Two},
        {5, Plural::Many, Plural::Many, Plural::Other, Plural::Few},
        {11, Plural::Many, Plural::Many, Plural::Other, Plural::Many},
        {12, Plural::Many, Plural::Many, Plural::Other, Plural::Many},
        {21, Plural::One, Plural::Many, Plural::Other, Plural::Many},
        {22, Plural::Few, Plural::Few, Plural::Other, Plural::Many},
        {100, Plural::Many, Plural::Many, Plural::Other, Plural::Other},
        {0, Plural::Many, Plural::Many, Plural::Other, Plural::Zero},
    };
    for (const auto &c : cases) {
        CHECK_EQ(std::string(plural_name(plural_category("ru", c.n))), std::string(plural_name(c.ru)));
        CHECK_EQ(std::string(plural_name(plural_category("pl", c.n))), std::string(plural_name(c.pl)));
        CHECK_EQ(std::string(plural_name(plural_category("cs", c.n))), std::string(plural_name(c.cs)));
        CHECK_EQ(std::string(plural_name(plural_category("ar", c.n))), std::string(plural_name(c.ar)));
    }
    CHECK(plural_category("sl", 102) == Plural::Two);
    CHECK(plural_category("lv", 11) == Plural::Zero);
    CHECK(plural_category("ro", 0) == Plural::Few);
    CHECK(plural_category("ru", -21) == Plural::One);  // a negative count takes its magnitude's form
    CHECK(plural_category("xx", 1) == Plural::One);    // unknown language: English rule
}

TEST("numbers use the locale's grouping and decimal separators") {
    CHECK_EQ(format_number(int64_t{1234567}, Locale::parse("en-US")), std::string("1,234,567"));
    CHECK_EQ(format_number(int64_t{1234567}, Locale::parse("de-DE")), std::string("1.234.567"));
    CHECK_EQ(format_number(int64_t{1234567}, Locale::parse("fr-FR")), std::string("1\xE2\x80\xAF" "234\xE2\x80\xAF" "567"));
    CHECK_EQ(format_number(int64_t{1234567}, Locale::parse("ru")), std::string("1\xC2\xA0" "234\xC2\xA0" "567"));
    CHECK_EQ(format_number(int64_t{1234567}, Locale::parse("de-CH")), std::string("1\xE2\x80\x99" "234\xE2\x80\x99" "567"));
    CHECK_EQ(format_number(int64_t{1234}, Locale::parse("es")), std::string("1234"));  // grouping from five digits
    CHECK_EQ(format_number(int64_t{12345}, Locale::parse("es")), std::string("12.345"));
    CHECK_EQ(format_number(int64_t{999}, Locale::parse("en")), std::string("999"));
    CHECK_EQ(format_number(int64_t{-1000}, Locale::parse("en")), std::string("-1,000"));
    CHECK_EQ(format_number(INT64_MIN, Locale::parse("en")), std::string("-9,223,372,036,854,775,808"));
    CHECK_EQ(format_number(1234.5, 1, Locale::parse("de")), std::string("1.234,5"));
    CHECK_EQ(format_number(0.125, 2, Locale::parse("en")), std::string("0.13"));
    CHECK_EQ(format_number(-0.001, 2, Locale::parse("en")), std::string("0.00"));
    CHECK_EQ(format_number(8.4, 1, Locale::parse("fr")), std::string("8,4"));
    CHECK_EQ(format_number(3.0, 0, Locale::parse("en")), std::string("3"));
}

TEST("settingsservice localeInfo: UI and formatting locales, refused or malformed replies") {
    Locale ui, fmt;
    CHECK(parse_locale_info(R"({"returnValue":true,"settings":{"localeInfo":{"locales":{"UI":"be_BY","FMT":"es-ES"},"clock":"12"}}})",
                            &ui, &fmt));
    CHECK_EQ(ui.tag(), std::string("be-BY"));
    CHECK_EQ(fmt.tag(), std::string("es-ES"));
    CHECK(parse_locale_info(R"({"settings":{"localeInfo":{"locales":{"UI":"ko-KR"}}}})", &ui, &fmt));
    CHECK_EQ(fmt.tag(), std::string("ko-KR"));  // no FMT: the UI locale
    CHECK(!parse_locale_info(R"({"returnValue":false,"errorText":"denied"})", &ui, &fmt));
    CHECK(!parse_locale_info("not json", &ui, &fmt));
    CHECK(!parse_locale_info(R"({"settings":{"localeInfo":{"locales":{"UI":5}}}})", &ui, &fmt));
}

TEST("catalog: translations, source fallback, placeholders and plural forms") {
    Catalog fr(Locale::parse("fr-FR"));
    std::string err;
    CHECK(fr.add_json(R"({"Search by name":"Rechercher par nom",
                          "Hello {name}":"Bonjour {name}",
                          "{n} show":{"one":"{n} série","other":"{n} séries"},
                          "bad":5})",
                      &err));
    CHECK(!err.empty());  // the bad entry is named, the others are kept
    CHECK_EQ(fr.size(), size_t{3});
    CHECK_EQ(fr.tr("Search by name"), std::string("Rechercher par nom"));
    CHECK_EQ(fr.tr("Not translated"), std::string("Not translated"));
    CHECK_EQ(fr.tr("Hello {name}", {{"name", "Ana"}}), std::string("Bonjour Ana"));
    CHECK_EQ(fr.tr("{unknown} stays"), std::string("{unknown} stays"));
    CHECK_EQ(fr.trn("{n} show", "{n} shows", 0), std::string("0 série"));  // French: 0 is singular
    CHECK_EQ(fr.trn("{n} show", "{n} shows", 2500), std::string("2\xE2\x80\xAF" "500 séries"));
    CHECK_EQ(fr.trn("{n} episode", "{n} episodes", 1), std::string("1 episode"));  // untranslated: English rule
    CHECK_EQ(fr.trn("{n} episode", "{n} episodes", 3), std::string("3 episodes"));
    CHECK(!fr.add_json("[1,2]", &err));

    Catalog ru(Locale::parse("ru"));
    ru.add_json(R"({"{n} show":{"one":"{n} сериал","few":"{n} сериала","many":"{n} сериалов"}})");
    CHECK_EQ(ru.trn("{n} show", "{n} shows", 21), std::string("21 сериал"));
    CHECK_EQ(ru.trn("{n} show", "{n} shows", 3), std::string("3 сериала"));
    CHECK_EQ(ru.trn("{n} show", "{n} shows", 11), std::string("11 сериалов"));
}

TEST("catalog files: the most specific file wins, missing files are fine") {
    char tmpl[] = "/tmp/wnk-i18n-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    auto write = [&dir](const char *name, const char *text) {
        std::FILE *f = std::fopen((dir + "/" + name).c_str(), "w");
        std::fputs(text, f);
        std::fclose(f);
    };
    write("zh-Hant.json", R"({"Search":"搜尋","Back":"返回"})");
    write("zh-TW.json", R"({"Search":"搜尋節目"})");
    write("zh.json", "this is not json");
    Catalog c(Locale::parse("zh_TW"));
    std::string err;
    CHECK_EQ(c.load_dir(dir, &err), 2);
    CHECK(err.find("zh.json") != std::string::npos);
    CHECK_EQ(c.tr("Search"), std::string("搜尋節目"));
    CHECK_EQ(c.tr("Back"), std::string("返回"));
    Catalog en(Locale::parse("en-GB"));
    CHECK_EQ(en.load_dir(dir), 0);
    CHECK_EQ(en.tr("Search"), std::string("Search"));
    std::system(("rm -rf '" + dir + "'").c_str());
}
