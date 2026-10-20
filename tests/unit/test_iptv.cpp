// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/iptv.hpp"
#include "wnk/media/playback_headers.hpp"

using namespace wnk::media;

namespace {
std::string get(const wnk::net::Headers &h, const char *name) {
    const std::string *v = header_get(h, name);
    return v ? *v : std::string("<none>");
}
}  // namespace

// ---- headers -----------------------------------------------------------------------------------

TEST("header_set treats names case-insensitively and replaces in place") {
    wnk::net::Headers h{{"Accept", "x"}};
    header_set(&h, "referer", "a");
    header_set(&h, "Referer", "b");
    REQUIRE(h.size() == size_t{2});
    CHECK_EQ(h[1].first, std::string("Referer"));
    CHECK_EQ(h[1].second, std::string("b"));
    CHECK_EQ(get(h, "REFERER"), std::string("b"));
    h.emplace_back("referer", "dup");
    header_set(&h, "REFERER", "c");
    CHECK_EQ(h.size(), size_t{2});
    CHECK_EQ(h[1].second, std::string("c"));
    CHECK(header_remove(&h, "referer"));
    CHECK(!header_remove(&h, "referer"));
    CHECK(header_get(h, "Referer") == nullptr);
}

TEST("parse_playback_link splits the URL and its headers") {
    const PlaybackLink l = parse_playback_link("https://h/x.m3u8|Referer=https://a/|User-Agent=U=V||=skip|bare");
    CHECK_EQ(l.url, std::string("https://h/x.m3u8"));
    REQUIRE(l.headers.size() == size_t{3});
    CHECK_EQ(l.headers[0].first, std::string("Referer"));
    CHECK_EQ(l.headers[0].second, std::string("https://a/"));
    CHECK_EQ(get(l.headers, "user-agent"), std::string("U=V"));
    CHECK_EQ(get(l.headers, "bare"), std::string(""));
    CHECK(parse_playback_link("http://h/x").headers.empty());
}

TEST("decode_path_slashes decodes %2F in the path only") {
    CHECK_EQ(decode_path_slashes("http://h%2Flive%2Fx.m3u8"), std::string("http://h/live/x.m3u8"));
    CHECK_EQ(decode_path_slashes("http://h/a%2fb?u=http%3A%2F%2Fx"), std::string("http://h/a/b?u=http%3A%2F%2Fx"));
    CHECK_EQ(decode_path_slashes("http://h/%2"), std::string("http://h/%2"));
}

TEST("normalize_playback_headers adds User-Agent and Accept only when missing") {
    wnk::net::Headers h{{"user-agent", "Mine"}};
    normalize_playback_headers(&h, kIptvUserAgent);
    CHECK_EQ(h.size(), size_t{2});
    CHECK_EQ(get(h, "User-Agent"), std::string("Mine"));
    CHECK_EQ(get(h, "Accept"), std::string("*/*"));
    wnk::net::Headers e;
    normalize_playback_headers(&e, kIptvUserAgent);
    CHECK_EQ(get(e, "User-Agent"), std::string(kIptvUserAgent));
    normalize_playback_headers(&e, kIptvUserAgent);
    CHECK_EQ(e.size(), size_t{2});
}

// ---- channel lists -----------------------------------------------------------------------------

TEST("iptv: EXTVLCOPT and URL parameters, then EXTHTTP; nothing leaks between entries") {
    const std::string list = "\xEF\xBB\xBF#EXTM3U url-tvg=\"http://epg/\"\r\n"
                             "#EXTINF:-1 tvg-id=\"one.uk\" tvg-logo=\"http://l/1.png\" group-title=\"News, UK\",One\r\n"
                             "#EXTVLCOPT:http-referrer=https://ref/\r\n"
                             "#EXTVLCOPT:http-user-agent=Box/1\r\n"
                             "#EXTVLCOPT:http-origin=https://org\r\n"
                             "https://h/live/one.m3u8?token=1|Cookie=a=b&user-agent=Box/2\r\n"
                             "\r\n"
                             "#EXTINF:-1 group-title=\"Sport\",Two\n"
                             "#EXTHTTP:{\"referer\":\"https://r2/\",\"cookie\":\"c=d\",\"n\":1}\n"
                             "https://h/live/two.m3u8\n";
    std::vector<IptvChannel> ch;
    std::string err;
    REQUIRE(parse_iptv_playlist(list, &ch, &err));
    REQUIRE(ch.size() == size_t{2});
    CHECK_EQ(ch[0].title, std::string("One"));
    CHECK_EQ(ch[0].tvg_id, std::string("one.uk"));
    CHECK_EQ(ch[0].logo, std::string("http://l/1.png"));
    CHECK_EQ(ch[0].group, std::string("News, UK"));
    CHECK_EQ(ch[0].url, std::string("https://h/live/one.m3u8?token=1"));
    CHECK_EQ(get(ch[0].headers, "Referer"), std::string("https://ref/"));
    CHECK_EQ(get(ch[0].headers, "Origin"), std::string("https://org"));
    CHECK_EQ(get(ch[0].headers, "User-Agent"), std::string("Box/2"));  // the URL parameter wins
    CHECK_EQ(get(ch[0].headers, "Cookie"), std::string("a=b"));
    CHECK_EQ(ch[0].headers.size(), size_t{4});
    CHECK_EQ(ch[0].headers[0].first, std::string("Referer"));

    CHECK_EQ(ch[1].title, std::string("Two"));
    CHECK_EQ(ch[1].group, std::string("Sport"));
    CHECK_EQ(ch[1].url, std::string("https://h/live/two.m3u8"));
    CHECK_EQ(get(ch[1].headers, "Referer"), std::string("https://r2/"));
    CHECK_EQ(get(ch[1].headers, "Cookie"), std::string("c=d"));
    CHECK(header_get(ch[1].headers, "Origin") == nullptr);
    CHECK(header_get(ch[1].headers, "User-Agent") == nullptr);
    CHECK_EQ(ch[1].headers.size(), size_t{2});  // "n" is not a string
}

TEST("iptv: the title is everything after the first comma outside quotes") {
    std::vector<IptvChannel> ch;
    std::string err;
    REQUIRE(parse_iptv_playlist("#EXTM3U\n#EXTINF:-1,News, Weather\nhttp://h/a.ts\n", &ch, &err));
    REQUIRE(ch.size() == size_t{1});
    CHECK_EQ(ch[0].title, std::string("News, Weather"));
    CHECK(ch[0].headers.empty());
}

TEST("iptv: wrapped URLs, encoded slashes, keys and licences") {
    const std::string list = "#EXTM3U\n"
                             "#EXTINF:-1 keyid=\"00112233445566778899aabbccddeeff\" drm-key=\"ffeeddccbbaa99887766554433221100\",A\n"
                             "http://h%2Flive\n"
                             "%2Fa.mpd\n"
                             "#EXTINF:-1,B\n"
                             "#KODIPROP:inputstream.adaptive.license_key=https://lic/ck\n"
                             "http://h/b.mpd\n"
                             "#EXTINF:-1,C\n"
                             "#KODIPROP:inputstream.adaptive.license_key={\"keys\":[]}\n"
                             "http://h/c.mpd\n"
                             "#EXTINF:-1,No URL\n";
    std::vector<IptvChannel> ch;
    std::string err;
    REQUIRE(parse_iptv_playlist(list, &ch, &err));
    REQUIRE(ch.size() == size_t{3});
    CHECK_EQ(ch[0].url, std::string("http://h/live/a.mpd"));
    CHECK_EQ(ch[0].keys_text, std::string("00112233445566778899aabbccddeeff:ffeeddccbbaa99887766554433221100"));
    CHECK_EQ(ch[1].license_url, std::string("https://lic/ck"));
    CHECK(ch[1].keys_text.empty());
    CHECK_EQ(ch[2].keys_text, std::string("{\"keys\":[]}"));
    CHECK(ch[2].license_url.empty());
}

TEST("iptv: HLS media playlists and lists without entries are refused with a reason") {
    std::vector<IptvChannel> ch;
    std::string err;
    CHECK(!parse_iptv_playlist("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nseg1.ts\n", &ch, &err));
    CHECK(err.find("HLS media playlist") != std::string::npos);
    CHECK(err.find("AdaptiveSource") != std::string::npos);
    CHECK(!parse_iptv_playlist("#EXTM3U\n#EXTINF:-1,Empty\n", &ch, &err));
    CHECK(!err.empty());
    CHECK(!parse_iptv_playlist("<MPD/>", &ch, &err));
    CHECK(!parse_iptv_playlist("", &ch, &err));
    CHECK(ch.empty());
}
