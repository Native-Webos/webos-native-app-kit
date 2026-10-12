// SPDX-License-Identifier: GPL-3.0-or-later
//
// Example C's catalogue code (applications/examples/shows/catalogue.cpp): parsing what the TVmaze API
// returns, including the malformed replies a public API can produce, the saved-copy encoding that
// must fit a DB8 value, and the error wording.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "catalogue.hpp"

#include <string>
#include <vector>

using namespace shows;
using wnk::net::NetError;

TEST("parses a page and skips unusable entries") {
    const std::string page = R"([
        {"id":1,"name":"Under the Dome","premiered":"2013-06-24","rating":{"average":6.6},"weight":100,
         "updated":1789071819,"image":{"medium":"https://static.tvmaze.com/a.jpg","original":"x"}},
        {"id":2,"name":"No Rating","premiered":null,"rating":{"average":null},"image":null,"weight":"high"},
        {"id":0,"name":"No id"},
        {"id":4},
        "not an object",
        {"id":5,"name":"Local image","image":{"medium":"file:///etc/passwd"},"rating":{"average":42}}
    ])";
    std::vector<Show> out;
    std::string why;
    REQUIRE(parse_show_list(page, &out, &why));
    REQUIRE(out.size() == 3u);
    CHECK_EQ(out[0].id, 1);
    CHECK_EQ(out[0].name, std::string("Under the Dome"));
    CHECK_EQ(out[0].year, 2013);
    CHECK(out[0].rating > 6.55f && out[0].rating < 6.65f);
    CHECK_EQ(out[0].weight, 100);
    CHECK_EQ(out[0].updated, 1789071819);
    CHECK_EQ(out[0].image, std::string("https://static.tvmaze.com/a.jpg"));
    CHECK_EQ(out[1].year, 0);
    CHECK_EQ(out[1].rating, 0.0f);
    CHECK_EQ(out[1].weight, 0);  // wrong type is "absent", not an exception
    CHECK(out[1].image.empty());
    CHECK(out[2].image.empty());  // file:// from the network is dropped
    CHECK_EQ(out[2].rating, 0.0f);  // out of range
}

TEST("rejects documents that are not a show list") {
    std::vector<Show> out;
    std::string why;
    CHECK(!parse_show_list("<html>maintenance</html>", &out, &why));
    CHECK_EQ(why, std::string("not JSON"));
    CHECK(!parse_show_list(R"({"name":"Not Found","status":404})", &out, &why));
    CHECK(parse_show_list("[]", &out, &why));
    CHECK(out.empty());
}

TEST("parses details with embedded seasons and cast") {
    const std::string doc = R"({"id":1,"name":"Under the Dome","status":"Ended","language":"English",
        "genres":["Drama","Science-Fiction",3],"runtime":null,"averageRuntime":60,"premiered":"2013-06-24",
        "ended":"2015-09-10","network":null,"webChannel":{"name":"Stream"},
        "summary":"<p><b>Under the Dome</b> is the story &amp; more.</p>",
        "_embedded":{"seasons":[{"number":1,"episodeOrder":13,"premiereDate":"2013-06-24"},
                                {"number":2,"episodeOrder":null},{"number":0}],
                     "cast":[{"person":{"name":"Mike Vogel"},"character":{"name":"Dale Barbara"}},
                             {"person":{"name":"Rachelle Lefevre"},"character":null},{"person":null}]}})";
    Details d;
    std::string why;
    REQUIRE(parse_details(doc, &d, &why));
    CHECK_EQ(d.show.id, 1);
    CHECK_EQ(d.summary, std::string("Under the Dome is the story & more."));
    CHECK_EQ(d.runtime, 60);
    CHECK_EQ(d.network, std::string("Stream"));
    CHECK_EQ(d.genres.size(), 2u);
    REQUIRE(d.seasons.size() == 2u);
    CHECK_EQ(d.seasons[0].episodes, 13);
    CHECK_EQ(d.seasons[1].episodes, 0);
    REQUIRE(d.cast.size() == 2u);
    CHECK_EQ(d.cast[0].character, std::string("Dale Barbara"));
    CHECK(d.cast[1].character.empty());
    CHECK(!parse_details(R"({"name":"no id"})", &d, &why));
}

TEST("strip html keeps paragraphs and decodes entities") {
    CHECK_EQ(strip_html("<p>One  <i>two</i>\n three.</p><p>Four&nbsp;&#233;&#x2014;&hellip;</p>"),
             std::string("One two three.\n\nFour \xC3\xA9\xE2\x80\x94\xE2\x80\xA6"));
    CHECK_EQ(strip_html("a<br>b<br/>c"), std::string("a\n\nb\n\nc"));
    CHECK_EQ(strip_html("AT&T &unknown; &#0; &"), std::string("AT&T &unknown; \xEF\xBF\xBD &"));
    CHECK_EQ(strip_html("cut <b"), std::string("cut"));
    CHECK_EQ(strip_html(""), std::string());
}

TEST("saved copy round trips and fits the value limit") {
    std::vector<Show> list;
    for (int i = 1; i <= 400; ++i) {
        Show s;
        s.id = i;
        s.name = "Show with a fairly long name " + std::to_string(i);
        s.image = "https://static.tvmaze.com/uploads/images/medium_portrait/123/" + std::to_string(i) + ".jpg";
        s.rating = 7.25f;
        s.weight = i % 100;
        s.year = 2000 + i % 20;
        s.updated = 1700000000 + i;
        list.push_back(s);
    }
    size_t kept = 0;
    const std::string text = encode_shows(list, 1234, 16 * 1024, &kept);
    CHECK(text.size() <= 16 * 1024);
    CHECK(kept > 50 && kept < 400);
    std::vector<Show> back;
    int64_t saved = 0;
    REQUIRE(decode_shows(text, &back, &saved));
    CHECK_EQ(saved, 1234);
    REQUIRE(back.size() == kept);
    CHECK_EQ(back[0].name, list[0].name);  // the front of the list is what survives
    CHECK_EQ(back[0].image, list[0].image);
    CHECK(back[0].rating > 7.29f && back[0].rating < 7.31f);  // one decimal is kept
    CHECK_EQ(back.back().id, static_cast<int>(kept));
    CHECK(!decode_shows("{}", &back, &saved));
    CHECK(!decode_shows(R"({"v":2,"shows":[]})", &back, &saved));
    CHECK(!decode_shows("garbage", &back, &saved));
}

TEST("favourites are most recent first and capped") {
    std::vector<Show> fav;
    Show a, b, c;
    a.id = 1, b.id = 2, c.id = 3;
    a.name = "a", b.name = "b", c.name = "c";
    set_favourite(&fav, a, true, 2);
    set_favourite(&fav, b, true, 2);
    set_favourite(&fav, a, true, 2);  // again: moves to the front, no duplicate
    REQUIRE(fav.size() == 2u);
    CHECK_EQ(fav[0].id, 1);
    set_favourite(&fav, c, true, 2);
    CHECK_EQ(fav.size(), 2u);
    CHECK(!contains(fav, 2));
    set_favourite(&fav, c, false, 2);
    CHECK_EQ(fav.size(), 1u);
    CHECK(contains(fav, 1));
}

TEST("rails sort stably and skip missing keys") {
    std::vector<Show> all(5);
    for (int i = 0; i < 5; ++i) all[i].id = i + 1;
    all[0].rating = 7, all[1].rating = 9, all[2].rating = 7, all[3].rating = 0, all[4].rating = 8;
    const auto top = top_rated(all, 3);
    REQUIRE(top.size() == 3u);
    CHECK_EQ(top[0].id, 2);
    CHECK_EQ(top[1].id, 5);
    CHECK_EQ(top[2].id, 1);  // ties keep API order
    CHECK_EQ(top_rated(all, 10).size(), 4u);  // unrated left out
    CHECK(recently_updated(all, 10).empty());
    CHECK_EQ(most_popular(all, 10).size(), 5u);
}

TEST("failures read as sentences") {
    CHECK_EQ(describe_failure(NetError::None, 503),
             std::string("The catalogue server has a problem (HTTP 503)."));
    CHECK_EQ(describe_failure(NetError::Dns, 0),
             std::string("Cannot reach the catalogue. Check the TV's network connection."));
    CHECK(describe_failure(NetError::Tls, 0).find("date and time") != std::string::npos);
    CHECK(is_transient(NetError::Timeout, 0));
    CHECK(is_transient(NetError::None, 502));
    CHECK(!is_transient(NetError::None, 404));
    CHECK(!is_transient(NetError::Tls, 0));
}

TEST("urls and base validation") {
    CHECK(valid_base("https://api.tvmaze.com"));
    CHECK(valid_base("http://192.168.50.251:8099/catalogue"));
    CHECK(!valid_base("https://api.tvmaze.com/"));
    CHECK(!valid_base("file:///etc"));
    CHECK(!valid_base("https://x/?a=b"));
    CHECK(!valid_base("https://"));
    CHECK_EQ(page_url("https://h", 3), std::string("https://h/shows?page=3"));
    CHECK_EQ(details_url("https://h", 7), std::string("https://h/shows/7?embed%5B%5D=seasons&embed%5B%5D=cast"));
}

TEST("age and card text") {
    CHECK_EQ(age_text(5), std::string("just now"));
    CHECK_EQ(age_text(600), std::string("10 min ago"));
    CHECK_EQ(age_text(7200), std::string("2 h ago"));
    CHECK_EQ(age_text(86400), std::string("1 day ago"));
    CHECK_EQ(age_text(-5), std::string());
    Show s;
    CHECK_EQ(card_subtitle(s), std::string());
    s.year = 2013;
    CHECK_EQ(card_subtitle(s), std::string("2013"));
    s.rating = 6.6f;
    CHECK_EQ(card_subtitle(s), std::string("2013 · 6.6"));
}
