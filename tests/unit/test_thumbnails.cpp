// SPDX-License-Identifier: GPL-3.0-or-later
// Seek previews: DASH thumbnail_tile AdaptationSets, HLS image playlists, WebVTT thumbnail files, and
// which tile of which sheet stands for a time.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/manifest.hpp"
#include "wnk/media/thumbnails.hpp"

#include <fstream>
#include <sstream>

using namespace wnk::media;

namespace {

std::string fixture(const std::string &name) {
    std::ifstream f(std::string(WNK_FIXTURES_DIR) + "/" + name, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// The DASH-IF IOP example: 100 s sheets of 10x1 tiles, 3200x180 each, beside a video.
const char *kMpd = R"(<?xml version="1.0"?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT250S">
 <Period id="0" start="PT0S">
  <AdaptationSet id="0" contentType="video" mimeType="video/mp4">
   <SegmentTemplate timescale="1000" duration="2000" initialization="v-init.mp4" media="v-$Number$.m4s" startNumber="1"/>
   <Representation id="v" codecs="avc1.64001f" bandwidth="1000000" width="1280" height="720"/>
  </AdaptationSet>
  <AdaptationSet id="3" mimeType="image/jpeg" contentType="image">
   <SegmentTemplate media="thumbs/$RepresentationID$/tile_$Number$.jpg" timescale="1" duration="100" startNumber="1"/>
   <Representation bandwidth="12288" id="thumbnails_320x180" width="3200" height="180">
    <EssentialProperty schemeIdUri="http://dashif.org/thumbnail_tile" value="10x1"/>
   </Representation>
   <Representation bandwidth="4096" id="thumbnails_160x90" width="800" height="270">
    <EssentialProperty schemeIdUri="http://dashif.org/guidelines/thumbnail_tile" value="5x3"/>
   </Representation>
  </AdaptationSet>
  <AdaptationSet id="4" mimeType="image/jpeg" contentType="image">
   <SegmentTemplate media="poster.jpg" duration="250"/>
   <Representation bandwidth="1000" id="poster" width="640" height="360"/>
  </AdaptationSet>
 </Period>
</MPD>)";

}  // namespace

TEST("thumbnails: tile layouts") {
    int c = 0, r = 0;
    CHECK(parse_tile_layout("10x1", &c, &r));
    CHECK_EQ(c, 10);
    CHECK_EQ(r, 1);
    CHECK(parse_tile_layout(" 5X4 ", &c, &r));
    CHECK_EQ(r, 4);
    CHECK(!parse_tile_layout("x4", &c, &r));
    CHECK(!parse_tile_layout("0x4", &c, &r));
    CHECK(!parse_tile_layout("5x", &c, &r));
    CHECK(!parse_tile_layout("5x-1", &c, &r));
    CHECK(!parse_tile_layout("five", &c, &r));
}

TEST("thumbnails: DASH thumbnail_tile Representations become tracks, not renditions") {
    Presentation p;
    std::string err;
    REQUIRE(parse_mpd(kMpd, "https://cdn.example/film/manifest.mpd", &p, &err));
    for (const Rendition &r : p.renditions) CHECK(r.id != "thumbnails_320x180" && r.id != "thumbnails_160x90");
    REQUIRE(p.thumbnails.size() == 2);
    const ThumbnailTrack &big = p.thumbnails[0];
    CHECK_EQ(big.tile_width, 320);
    CHECK_EQ(big.tile_height, 180);
    REQUIRE(big.sheets.size() == 3);  // 250 s in 100 s sheets
    CHECK_EQ(big.sheets[1].url, std::string("https://cdn.example/film/thumbs/thumbnails_320x180/tile_2.jpg"));
    CHECK_EQ(big.sheets[1].start_us, int64_t{100000000});
    CHECK_EQ(big.sheets[1].duration_us, int64_t{100000000});
    // 135 s: sheet 2, tile 3 (10 s each) → x = 3 * 320.
    auto t = big.at(135000000);
    REQUIRE(t.has_value());
    CHECK_EQ(t->url, big.sheets[1].url);
    CHECK_EQ(t->x, 960);
    CHECK_EQ(t->y, 0);
    CHECK_EQ(t->width, 320);
    CHECK_EQ(t->height, 180);
    CHECK_EQ(t->start_us, int64_t{130000000});
    CHECK_EQ(t->end_us, int64_t{140000000});
    // A grid: 5x3 tiles of 160x90 over 100 s (6.67 s each); 60 s is tile 9 → column 4, row 1.
    const ThumbnailTrack &small = p.thumbnails[1];
    CHECK_EQ(small.tile_width, 160);
    auto g = small.at(60000000);
    REQUIRE(g.has_value());
    CHECK_EQ(g->x, 640);
    CHECK_EQ(g->y, 90);
    CHECK(!big.at(-1).has_value());
    CHECK(!big.at(300000000).has_value());  // past the last sheet
    CHECK_EQ(choose_thumbnail_track(p.thumbnails, 320), 0);
    CHECK_EQ(choose_thumbnail_track(p.thumbnails, 200), 1);
    CHECK_EQ(choose_thumbnail_track(p.thumbnails, 100), 1);  // none fits: the smallest
    CHECK_EQ(choose_thumbnail_track({}, 320), -1);
}

TEST("thumbnails: HLS image playlists (EXT-X-IMAGE-STREAM-INF, EXT-X-TILES)") {
    const std::string master =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=1280x720,CODECS=\"avc1.64001f,mp4a.40.2\"\nvideo.m3u8\n"
        "#EXT-X-IMAGE-STREAM-INF:BANDWIDTH=16000,RESOLUTION=320x180,CODECS=\"jpeg\",URI=\"images/320/playlist.m3u8\"\n"
        "#EXT-X-IMAGE-STREAM-INF:BANDWIDTH=4000,RESOLUTION=160x90,CODECS=\"jpeg\",URI=\"images/160/playlist.m3u8\"\n";
    Presentation p;
    std::string err;
    REQUIRE(parse_hls(master, "https://cdn.example/film/master.m3u8", &p, &err));
    REQUIRE(p.image_playlists.size() == 2);
    CHECK_EQ(p.image_playlists[0].url, std::string("https://cdn.example/film/images/320/playlist.m3u8"));
    CHECK_EQ(p.image_playlists[0].width, 320);
    CHECK_EQ(p.image_playlists[1].bandwidth, int64_t{4000});
    for (const Rendition &r : p.renditions) CHECK(r.playlist_url.find("images/") == std::string::npos);

    const std::string images =
        "#EXTM3U\n#EXT-X-TARGETDURATION:60\n#EXT-X-VERSION:7\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXT-X-IMAGES-ONLY\n"
        "#EXTINF:60.000,\n#EXT-X-TILES:RESOLUTION=320x180,LAYOUT=5x4,DURATION=3.000\ntile1.jpg\n"
        "#EXTINF:30.000,\n#EXT-X-TILES:RESOLUTION=320x180,LAYOUT=5x4,DURATION=3.000\ntile2.jpg\n"
        "#EXT-X-ENDLIST\n";
    ThumbnailTrack t;
    REQUIRE(parse_hls_image_playlist(images, "https://cdn.example/film/images/320/playlist.m3u8", &t, &err));
    REQUIRE(t.sheets.size() == 2);
    CHECK_EQ(t.tile_width, 320);
    CHECK_EQ(t.sheets[1].start_us, int64_t{60000000});
    CHECK_EQ(t.sheets[1].url, std::string("https://cdn.example/film/images/320/tile2.jpg"));
    // 20 s: tile 6 of a 5x4 sheet (3 s each) → column 1, row 1.
    auto a = t.at(20000000);
    REQUIRE(a.has_value());
    CHECK_EQ(a->x, 320);
    CHECK_EQ(a->y, 180);
    CHECK_EQ(a->start_us, int64_t{18000000});
    // 75 s: the second sheet, (75 - 60) / 3 = tile 5 → column 0, row 1.
    auto b = t.at(75000000);
    REQUIRE(b.has_value());
    CHECK_EQ(b->x, 0);
    CHECK_EQ(b->y, 180);
    // Without EXT-X-TILES each image is one picture for its whole EXTINF.
    ThumbnailTrack plain;
    REQUIRE(parse_hls_image_playlist("#EXTM3U\n#EXTINF:10,\na.jpg\n#EXTINF:10,\nb.jpg\n", "http://h/i.m3u8", &plain, &err));
    auto c = plain.at(15000000);
    REQUIRE(c.has_value());
    CHECK_EQ(c->url, std::string("http://h/b.jpg"));
    CHECK_EQ(c->width, 0);
    CHECK(!parse_hls_image_playlist("#EXTM3U\n#EXT-X-ENDLIST\n", "http://h/i.m3u8", &plain, &err));
}

TEST("thumbnails: a WebVTT thumbnails file") {
    ThumbnailTrack t;
    std::string err;
    REQUIRE(parse_thumbnail_vtt(fixture("thumbs/thumbs.vtt"), "http://h/media/thumbs.vtt", &t, &err));
    CHECK_EQ(t.sheets.size(), size_t{12});
    CHECK_EQ(t.tile_width, 160);
    auto a = t.at(27500000);
    REQUIRE(a.has_value());
    CHECK_EQ(a->url, std::string("http://h/media/sheet.jpg"));
    CHECK_EQ(a->x, 160);  // cue 6: column 1, row 1
    CHECK_EQ(a->y, 90);
    CHECK_EQ(a->width, 160);
    CHECK_EQ(a->start_us, int64_t{25000000});
    // Whole images, absolute URLs, cues out of order, a malformed region skipped.
    const std::string vtt = "WEBVTT\n\n00:00:10.000 --> 00:00:20.000\nhttps://img.example/b.jpg\n\n"
                            "00:00:00.000 --> 00:00:10.000\na.jpg\n\n"
                            "00:00:20.000 --> 00:00:30.000\nc.jpg#xywh=1,2,x,4\n";
    REQUIRE(parse_thumbnail_vtt(vtt, "http://h/t.vtt", &t, &err));
    REQUIRE(t.sheets.size() == 2);
    const auto first = t.at(5000000), second = t.at(15000000);  // kept: the macros hold references
    REQUIRE(first.has_value() && second.has_value());
    CHECK_EQ(first->url, std::string("http://h/a.jpg"));
    CHECK_EQ(second->url, std::string("https://img.example/b.jpg"));
    CHECK_EQ(second->width, 0);
    CHECK(!t.at(25000000).has_value());
    CHECK(!parse_thumbnail_vtt("not vtt", "http://h/t.vtt", &t, &err));
    CHECK(!parse_thumbnail_vtt("WEBVTT\n\n", "http://h/t.vtt", &t, &err));
}
