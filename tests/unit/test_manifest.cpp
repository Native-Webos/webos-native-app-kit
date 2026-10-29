// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/manifest.hpp"
#include "wnk/media/xml.hpp"

#include <cstring>

using namespace wnk::media;

// ---- XML ---------------------------------------------------------------------------------------

TEST("xml: elements, attributes, text, entities, CDATA, comments, prefixes") {
    const std::string doc = "\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!-- top -->\n<!DOCTYPE MPD>\n"
                            "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" xmlns:cenc=\"urn:mpeg:cenc:2013\" a='1 &amp; 2'>"
                            "<BaseURL>http://h/&#x41;&#66;/</BaseURL><!-- c --><cenc:pssh>AAA<![CDATA[<raw>]]></cenc:pssh>"
                            "<Empty/><Empty k=\"&lt;&gt;&quot;&apos;\"/></MPD>";
    XmlNode root;
    std::string err;
    REQUIRE(parse_xml(doc, &root, &err));
    CHECK_EQ(root.name, std::string("MPD"));
    CHECK_EQ(root.attribute_or("a", ""), std::string("1 & 2"));
    REQUIRE(root.child("BaseURL") != nullptr);
    CHECK_EQ(root.child("BaseURL")->text, std::string("http://h/AB/"));
    REQUIRE(root.child("pssh") != nullptr);
    CHECK_EQ(root.child("pssh")->name, std::string("cenc:pssh"));
    CHECK_EQ(root.child("pssh")->text, std::string("AAA<raw>"));
    CHECK_EQ(root.children_named("Empty").size(), size_t{2});
    CHECK_EQ(root.children_named("Empty")[1]->attribute_or("k", ""), std::string("<>\"'"));
    CHECK(root.attribute("cenc") == nullptr);  // xmlns:cenc is not an attribute named "cenc"
}

TEST("xml: text_children keeps mixed content in document order") {
    XmlLimits limits;
    limits.text_children = true;
    XmlNode root;
    std::string err;
    REQUIRE(parse_xml("<p>one <span>two</span><br/>th<!-- c -->ree<![CDATA[!]]></p>", &root, &err, limits));
    REQUIRE(root.children.size() == 4);
    CHECK_EQ(root.children[0].name, std::string("#text"));
    CHECK_EQ(root.children[0].text, std::string("one "));
    CHECK_EQ(root.children[1].local(), std::string("span"));
    CHECK_EQ(root.children[1].children[0].text, std::string("two"));
    CHECK_EQ(root.children[2].local(), std::string("br"));
    CHECK_EQ(root.children[3].text, std::string("three!"));  // a comment does not split a run
    CHECK_EQ(root.text, std::string("one three!"));            // still concatenated as without it
    REQUIRE(parse_xml("<p>one <span>two</span></p>", &root, &err));
    CHECK_EQ(root.children.size(), size_t{1});  // off by default
}

TEST("xml: malformed documents and hostile ones are refused with a reason") {
    XmlNode root;
    std::string err;
    CHECK(!parse_xml("<a><b></a>", &root, &err));
    CHECK(err.find("mismatched") != std::string::npos);
    CHECK(!parse_xml("<a x=1/>", &root, &err));
    CHECK(!parse_xml("<a>&ent;</a>", &root, &err));  // no DTD, no custom entities
    CHECK(!parse_xml("<!DOCTYPE a [<!ENTITY e SYSTEM \"file:///etc/passwd\">]><a>&e;</a>", &root, &err));
    CHECK(err.find("internal subset") != std::string::npos);
    CHECK(!parse_xml("<a></a><b/>", &root, &err));
    CHECK(!parse_xml("", &root, &err));
    std::string deep;
    for (int i = 0; i < 100; ++i) deep += "<a>";
    CHECK(!parse_xml(deep, &root, &err));
    CHECK(err.find("deep") != std::string::npos);
}

// ---- URLs, durations, codecs -------------------------------------------------------------------

TEST("resolve_url follows RFC 3986 section 5.4") {
    const std::string base = "http://a/b/c/d;p?q";
    CHECK_EQ(resolve_url(base, "g"), std::string("http://a/b/c/g"));
    CHECK_EQ(resolve_url(base, "./g"), std::string("http://a/b/c/g"));
    CHECK_EQ(resolve_url(base, "g/"), std::string("http://a/b/c/g/"));
    CHECK_EQ(resolve_url(base, "/g"), std::string("http://a/g"));
    CHECK_EQ(resolve_url(base, "//g"), std::string("http://g"));
    CHECK_EQ(resolve_url(base, "?y"), std::string("http://a/b/c/d;p?y"));
    CHECK_EQ(resolve_url(base, "g?y"), std::string("http://a/b/c/g?y"));
    CHECK_EQ(resolve_url(base, "#s"), std::string("http://a/b/c/d;p?q#s"));
    CHECK_EQ(resolve_url(base, ""), std::string("http://a/b/c/d;p?q"));
    CHECK_EQ(resolve_url(base, "."), std::string("http://a/b/c/"));
    CHECK_EQ(resolve_url(base, ".."), std::string("http://a/b/"));
    CHECK_EQ(resolve_url(base, "../g"), std::string("http://a/b/g"));
    CHECK_EQ(resolve_url(base, "../../g"), std::string("http://a/g"));
    CHECK_EQ(resolve_url(base, "../../../g"), std::string("http://a/g"));
    CHECK_EQ(resolve_url(base, "https://x/y/../z"), std::string("https://x/z"));
    CHECK_EQ(resolve_url("https://h/v/master.m3u8?token=1", "a/b.m3u8"), std::string("https://h/v/a/b.m3u8"));
    CHECK_EQ(resolve_url("https://h", "seg.ts"), std::string("https://h/seg.ts"));
}

TEST("ISO 8601 durations") {
    int64_t us = 0;
    REQUIRE(parse_iso_duration("PT1H2M3.5S", &us));
    CHECK_EQ(us, int64_t{3723500000});
    REQUIRE(parse_iso_duration("PT0S", &us));
    CHECK_EQ(us, int64_t{0});
    REQUIRE(parse_iso_duration("P1DT1S", &us));
    CHECK_EQ(us, int64_t{86401000000});
    REQUIRE(parse_iso_duration("PT634.566S", &us));
    CHECK_EQ(us, int64_t{634566000});
    CHECK(!parse_iso_duration("1H", &us));
    CHECK(!parse_iso_duration("PT5X", &us));
    CHECK(!parse_iso_duration("P", &us));
}

TEST("Dolby Vision profiles from codec strings") {
    CHECK_EQ(dolby_vision_profile("dvhe.08.06"), 8);
    CHECK_EQ(dolby_vision_profile("dvh1.05.06"), 5);
    CHECK_EQ(dolby_vision_profile("dav1.10.09"), 10);
    CHECK_EQ(dolby_vision_profile("hvc1.2.4.L153"), -1);
    CHECK_EQ(dolby_vision_profile("dvhe"), -1);
    CHECK_EQ(dolby_vision_profile(""), -1);
}

TEST("RFC 6381 codec strings map to the kit's codecs") {
    CHECK(codec_from_rfc6381("avc1.640028,mp4a.40.2", TrackType::Video) == CodecId::H264);
    CHECK(codec_from_rfc6381("avc1.640028,mp4a.40.2", TrackType::Audio) == CodecId::Aac);
    CHECK(codec_from_rfc6381("hvc1.2.4.L150.90", TrackType::Video) == CodecId::Hevc);
    CHECK(codec_from_rfc6381("dvhe.08.06", TrackType::Video) == CodecId::Hevc);
    CHECK(codec_from_rfc6381("ec-3", TrackType::Audio) == CodecId::Eac3);
    CHECK(codec_from_rfc6381("ac-3", TrackType::Audio) == CodecId::Ac3);
    CHECK(codec_from_rfc6381("mp4a.40.34", TrackType::Audio) == CodecId::Mp3);
    CHECK(codec_from_rfc6381("av01.0.08M.08", TrackType::Video) == CodecId::Av1);
    CHECK(codec_from_rfc6381("vp09.00.10.08", TrackType::Video) == CodecId::Vp9);
    CHECK(codec_from_rfc6381("stpp", TrackType::Video) == CodecId::Unknown);
}

// ---- DASH --------------------------------------------------------------------------------------

namespace {

// Shaped like the Dolby Vision + Atmos test stream (Bento4, static, SegmentTemplate by duration).
const char *kDolbyLike = R"(<?xml version="1.0" ?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" profiles="urn:mpeg:dash:profile:isoff-live:2011" minBufferTime="PT2.00S"
     mediaPresentationDuration="PT10.010S" type="static">
  <Period>
    <AdaptationSet mimeType="video/mp4" segmentAlignment="true" startWithSAP="1" maxWidth="3840" maxHeight="2160">
      <SegmentTemplate timescale="1000" duration="2002" initialization="$RepresentationID$/init.mp4"
                       media="$RepresentationID$/seg-$Number$.m4s" startNumber="1"/>
      <Representation id="video/avc1" codecs="hev1.2.4.L120.90" width="1920" height="1080" frameRate="60000/1001"
                      bandwidth="7000000"/>
      <Representation id="video/4k" codecs="hev1.2.4.L153.90" width="3840" height="2160" frameRate="60000/1001"
                      bandwidth="16000000"/>
    </AdaptationSet>
    <AdaptationSet mimeType="video/mp4">
      <SegmentTemplate timescale="1000" duration="2002" initialization="$RepresentationID$/init.mp4"
                       media="$RepresentationID$/seg-$Number$.m4s" startNumber="1"/>
      <Representation id="video/dv" codecs="dvhe.08.06" width="3840" height="2160" frameRate="60000/1001" bandwidth="16000000"/>
    </AdaptationSet>
    <AdaptationSet mimeType="audio/mp4" startWithSAP="1" segmentAlignment="true" lang="en">
      <SupplementalProperty schemeIdUri="tag:dolby.com,2018:dash:EC3_ExtensionType:2018" value="JOC"/>
      <SegmentTemplate timescale="1000" duration="2002" initialization="$RepresentationID$/init.mp4"
                       media="$RepresentationID$/seg-$Number$.m4s" startNumber="1"/>
      <Representation id="audio/en/ec-3" codecs="ec-3" bandwidth="768000" audioSamplingRate="48000">
        <AudioChannelConfiguration schemeIdUri="tag:dolby.com,2014:dash:audio_channel_configuration:2011" value="F801"/>
      </Representation>
    </AdaptationSet>
  </Period>
</MPD>)";

}  // namespace

TEST("dash: SegmentTemplate by duration, Dolby Vision and Atmos flags, Dolby channel mask") {
    Presentation p;
    std::string err;
    REQUIRE(parse_mpd(kDolbyLike, "https://media.example.com/dv/p8.1.mpd", &p, &err));
    CHECK(!p.live);
    CHECK_EQ(p.duration_us, int64_t{10010000});
    REQUIRE(p.renditions.size() == 4);
    const Rendition &v = p.renditions[0];
    CHECK(v.type == TrackType::Video);
    CHECK_EQ(v.width, 1920);
    CHECK(std::abs(v.frame_rate - 59.94) < 0.01);
    REQUIRE(v.init.has_value());
    CHECK_EQ(v.init->url, std::string("https://media.example.com/dv/video/avc1/init.mp4"));
    REQUIRE(v.segments.size() == 5);  // ceil(10.010 / 2.002)
    CHECK_EQ(v.segments[0].url, std::string("https://media.example.com/dv/video/avc1/seg-1.m4s"));
    CHECK_EQ(v.segments[4].url, std::string("https://media.example.com/dv/video/avc1/seg-5.m4s"));
    CHECK_EQ(v.segments[1].start_us, int64_t{2002000});
    CHECK_EQ(v.segments[4].duration_us, int64_t{2002000});
    CHECK(v.dolby_vision.empty());
    CHECK_EQ(p.renditions[2].dolby_vision, std::string("dvhe.08.06"));
    const Rendition &a = p.renditions[3];
    CHECK(a.type == TrackType::Audio);
    CHECK(a.atmos);
    CHECK_EQ(a.channels, 6);
    CHECK_EQ(a.sample_rate, 48000);
    CHECK_EQ(a.language, std::string("en"));
    CHECK(!a.encrypted);
}

TEST("dash: SegmentTimeline with $Time$, repeats, r=-1, nested BaseURLs, inheritance, protection") {
    const char *mpd = R"(<MPD type="static" mediaPresentationDuration="PT20S">
      <BaseURL>cdn/</BaseURL>
      <Period>
        <BaseURL>p1/</BaseURL>
        <AdaptationSet contentType="video" id="7">
          <SegmentTemplate timescale="90000" media="v_$Bandwidth$_$Time$.m4s" initialization="v_$Bandwidth$_init.mp4"
                           presentationTimeOffset="90000">
            <SegmentTimeline><S t="90000" d="360000" r="2"/><S d="180000" r="-1"/></SegmentTimeline>
          </SegmentTemplate>
          <Representation id="lo" bandwidth="500000" codecs="avc1.4d401f" width="640" height="360"/>
          <Representation id="hi" bandwidth="3000000" codecs="avc1.640028" width="1920" height="1080">
            <BaseURL>hi/</BaseURL>
          </Representation>
        </AdaptationSet>
        <AdaptationSet contentType="audio" lang="de">
          <ContentProtection schemeIdUri="urn:mpeg:dash:mp4protection:2011" value="cenc"/>
          <SegmentTemplate timescale="48000" duration="96000" media="a$Number%05d$.m4s" startNumber="0"/>
          <Representation id="a" bandwidth="128000" codecs="mp4a.40.2">
            <AudioChannelConfiguration schemeIdUri="urn:mpeg:dash:23003:3:audio_channel_configuration:2011" value="2"/>
          </Representation>
        </AdaptationSet>
      </Period></MPD>)";
    Presentation p;
    std::string err;
    REQUIRE(parse_mpd(mpd, "http://h/x/manifest.mpd", &p, &err));
    REQUIRE(p.renditions.size() == 3);
    const Rendition &lo = p.renditions[0];
    CHECK_EQ(lo.group, std::string("7"));
    REQUIRE(lo.init.has_value());
    CHECK_EQ(lo.init->url, std::string("http://h/x/cdn/p1/v_500000_init.mp4"));
    // 3 × 4 s from t=1 s (offset 1 s → start 0), then 2 s segments to the end of the 20 s period.
    REQUIRE(lo.segments.size() >= 7);
    CHECK_EQ(lo.segments[0].url, std::string("http://h/x/cdn/p1/v_500000_90000.m4s"));
    CHECK_EQ(lo.segments[0].start_us, int64_t{0});
    CHECK_EQ(lo.segments[2].start_us, int64_t{8000000});
    CHECK_EQ(lo.segments[3].url, std::string("http://h/x/cdn/p1/v_500000_1170000.m4s"));
    CHECK_EQ(lo.segments[3].duration_us, int64_t{2000000});
    CHECK_EQ(lo.segments.back().start_us + lo.segments.back().duration_us, int64_t{20000000});
    CHECK_EQ(p.renditions[1].segments[0].url, std::string("http://h/x/cdn/p1/hi/v_3000000_90000.m4s"));
    const Rendition &a = p.renditions[2];
    CHECK(a.encrypted);
    CHECK_EQ(a.channels, 2);
    CHECK_EQ(a.language, std::string("de"));
    REQUIRE(a.segments.size() == 10);
    CHECK_EQ(a.segments[0].url, std::string("http://h/x/cdn/p1/a00000.m4s"));
    CHECK_EQ(a.segments[9].url, std::string("http://h/x/cdn/p1/a00009.m4s"));
}

TEST("dash: SegmentBase with indexRange, SegmentList, and a single-file subtitle") {
    const char *mpd = R"(<MPD type="static" mediaPresentationDuration="PT8S"><Period>
      <AdaptationSet mimeType="video/mp4">
        <Representation id="v" bandwidth="1" codecs="avc1.64001f">
          <BaseURL>video.mp4</BaseURL>
          <SegmentBase indexRange="800-1023"><Initialization range="0-799"/></SegmentBase>
        </Representation>
      </AdaptationSet>
      <AdaptationSet mimeType="audio/mp4">
        <Representation id="a" bandwidth="1" codecs="mp4a.40.2">
          <SegmentList timescale="1000" duration="4000">
            <Initialization sourceURL="a/init.mp4"/>
            <SegmentURL media="a/1.m4s"/><SegmentURL media="a/2.m4s" mediaRange="10-19"/>
          </SegmentList>
        </Representation>
      </AdaptationSet>
      <AdaptationSet mimeType="text/vtt" lang="fr">
        <Representation id="s" bandwidth="1"><BaseURL>subs/fr.vtt</BaseURL></Representation>
      </AdaptationSet>
    </Period></MPD>)";
    Presentation p;
    std::string err;
    REQUIRE(parse_mpd(mpd, "https://h/m.mpd", &p, &err));
    REQUIRE(p.renditions.size() == 3);
    const Rendition &v = p.renditions[0];
    CHECK(v.needs_index());
    CHECK_EQ(v.index_url, std::string("https://h/video.mp4"));
    CHECK_EQ(v.index.offset, int64_t{800});
    CHECK_EQ(v.index.length, int64_t{224});
    REQUIRE(v.init.has_value());
    CHECK_EQ(v.init->url, std::string("https://h/video.mp4"));
    CHECK_EQ(v.init->range.length, int64_t{800});
    const Rendition &a = p.renditions[1];
    REQUIRE(a.segments.size() == 2);
    CHECK_EQ(a.init->url, std::string("https://h/a/init.mp4"));
    CHECK_EQ(a.segments[1].start_us, int64_t{4000000});
    CHECK_EQ(a.segments[1].range.offset, int64_t{10});
    CHECK_EQ(a.segments[1].range.length, int64_t{10});
    const Rendition &s = p.renditions[2];
    CHECK(s.type == TrackType::Subtitle);
    REQUIRE(s.segments.size() == 1);
    CHECK_EQ(s.segments[0].url, std::string("https://h/subs/fr.vtt"));
    CHECK(s.segments[0].range.whole());
}

TEST("dash: live MPDs are flagged; garbage and missing pieces are refused") {
    Presentation p;
    std::string err;
    REQUIRE(parse_mpd(R"(<MPD type="dynamic"><Period><AdaptationSet mimeType="video/mp4">
        <SegmentTemplate timescale="1" media="$Number$.m4s"><SegmentTimeline><S t="0" d="2" r="3"/></SegmentTimeline></SegmentTemplate>
        <Representation id="v" bandwidth="1"/></AdaptationSet></Period></MPD>)",
                      "http://h/l.mpd", &p, &err));
    CHECK(p.live);
    CHECK_EQ(p.renditions[0].segments.size(), size_t{4});
    CHECK(!parse_mpd("<html/>", "http://h/", &p, &err));
    CHECK(!parse_mpd("<MPD/>", "http://h/", &p, &err));
    CHECK(!parse_mpd(R"(<MPD><Period><AdaptationSet><SegmentTemplate media="$Number$"/>
        <Representation id="v"/></AdaptationSet></Period></MPD>)", "http://h/", &p, &err));
    CHECK(err.find("duration") != std::string::npos);
}

// ---- HLS ---------------------------------------------------------------------------------------

TEST("hls: master playlist with variants, audio and subtitle renditions") {
    const char *m3u8 = "#EXTM3U\r\n#EXT-X-VERSION:6\r\n"
                       "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",LANGUAGE=\"en\",NAME=\"English\",DEFAULT=YES,CHANNELS=\"16/JOC\",URI=\"audio/en.m3u8\"\r\n"
                       "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",LANGUAGE=\"fr\",NAME=\"Français, stéréo\",CHANNELS=\"2\",URI=\"audio/fr.m3u8\"\r\n"
                       "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",LANGUAGE=\"en\",NAME=\"English\",URI=\"subs/en.m3u8\"\r\n"
                       "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720,FRAME-RATE=29.970,CODECS=\"avc1.4d401f,ec-3\",AUDIO=\"aud\",SUBTITLES=\"subs\"\r\n"
                       "v720/index.m3u8\r\n"
                       "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=100000,URI=\"iframes.m3u8\"\r\n"
                       "#EXT-X-STREAM-INF:BANDWIDTH=6000000,RESOLUTION=1920x1080,CODECS=\"dvh1.05.06,ec-3\",AUDIO=\"aud\"\r\n"
                       "https://other.example.com/v1080.m3u8?sig=abc\r\n";
    Presentation p;
    std::string err;
    REQUIRE(parse_hls(m3u8, "https://h/vod/master.m3u8", &p, &err));
    CHECK(p.format == Presentation::Format::Hls);
    REQUIRE(p.renditions.size() == 5);
    const Rendition &en = p.renditions[0];
    CHECK(en.type == TrackType::Audio);
    CHECK(en.is_default && en.atmos);
    CHECK_EQ(en.channels, 16);
    CHECK_EQ(en.codecs, std::string("ec-3"));
    CHECK(en.needs_playlist());
    CHECK_EQ(en.playlist_url, std::string("https://h/vod/audio/en.m3u8"));
    CHECK_EQ(p.renditions[1].label, std::string("Français, stéréo"));
    CHECK(p.renditions[2].type == TrackType::Subtitle);
    const Rendition &v = p.renditions[3];
    CHECK(v.type == TrackType::Video);
    CHECK(!v.muxed_audio);  // its audio comes from the "aud" renditions
    CHECK_EQ(v.width, 1280);
    CHECK_EQ(v.bandwidth, int64_t{2000000});
    CHECK(std::abs(v.frame_rate - 29.97) < 0.001);
    CHECK_EQ(v.playlist_url, std::string("https://h/vod/v720/index.m3u8"));
    CHECK_EQ(p.renditions[4].dolby_vision, std::string("dvh1.05.06"));
    CHECK_EQ(p.renditions[4].playlist_url, std::string("https://other.example.com/v1080.m3u8?sig=abc"));
}

TEST("hls: variants without separate audio carry it themselves") {
    const char *m3u8 = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000,CODECS=\"avc1.42e01e,mp4a.40.2\"\nlow.m3u8\n"
                       "#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS=\"mp4a.40.2\"\naudio-only.m3u8\n";
    Presentation p;
    std::string err;
    REQUIRE(parse_hls(m3u8, "http://h/a/b.m3u8", &p, &err));
    REQUIRE(p.renditions.size() == 2);
    CHECK(p.renditions[0].muxed_audio);
    CHECK(p.renditions[1].type == TrackType::Audio);
}

TEST("hls: media playlist with fMP4 map, byte ranges, discontinuity, key, end list") {
    const char *m3u8 = "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:100\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                       "#EXT-X-MAP:URI=\"main.mp4\",BYTERANGE=\"720@0\"\n"
                       "#EXTINF:6.006,\n#EXT-X-BYTERANGE:1000@720\nmain.mp4\n"
                       "#EXTINF:6.006,\n#EXT-X-BYTERANGE:2000\nmain.mp4\n"
                       "#EXT-X-DISCONTINUITY\n#EXTINF:3.5,title\n../other/seg.m4s\n#EXT-X-ENDLIST\n";
    Rendition r;
    bool live = true;
    std::string err;
    REQUIRE(parse_hls_media(m3u8, "https://h/v/720.m3u8", &r, &live, &err));
    CHECK(!live);
    CHECK(!r.encrypted);
    REQUIRE(r.init.has_value());
    CHECK_EQ(r.init->url, std::string("https://h/v/main.mp4"));
    CHECK_EQ(r.init->range.length, int64_t{720});
    REQUIRE(r.segments.size() == 3);
    CHECK_EQ(r.segments[0].range.offset, int64_t{720});
    CHECK_EQ(r.segments[1].range.offset, int64_t{1720});  // continues after the previous range
    CHECK_EQ(r.segments[1].range.length, int64_t{2000});
    CHECK_EQ(r.segments[1].start_us, int64_t{6006000});
    CHECK(r.segments[2].discontinuity);
    CHECK_EQ(r.segments[2].url, std::string("https://h/other/seg.m4s"));
    CHECK(r.segments[2].range.whole());

    const char *live_enc = "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:4,\na.ts\n";
    REQUIRE(parse_hls_media(live_enc, "https://h/l.m3u8", &r, &live, &err));
    CHECK(live);
    CHECK(r.encrypted);
    CHECK(!parse_hls_media("#EXTM3U\na.ts\n", "https://h/", &r, &live, &err));
}

TEST("hls: a media playlist given directly is one muxed rendition with a duration") {
    Presentation p;
    std::string err;
    REQUIRE(parse_hls("#EXTM3U\n#EXTINF:10,\n0.ts\n#EXTINF:5.5,\n1.ts\n#EXT-X-ENDLIST\n", "http://h/p.m3u8", &p, &err));
    REQUIRE(p.renditions.size() == 1);
    CHECK(p.renditions[0].muxed_audio);
    CHECK_EQ(p.renditions[0].segments.size(), size_t{2});
    CHECK_EQ(p.duration_us, int64_t{15500000});
    CHECK(!parse_hls("<MPD/>", "http://h/", &p, &err));
}

// ---- sidx --------------------------------------------------------------------------------------

namespace {
void put32(std::vector<uint8_t> *b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b->push_back(static_cast<uint8_t>(v >> s));
}
}  // namespace

TEST("sidx: version 0 and 1 boxes give byte ranges and times; hierarchical ones are refused") {
    for (int version : {0, 1}) {
        std::vector<uint8_t> b;
        const uint32_t size = version == 0 ? 32 + 3 * 12 : 40 + 3 * 12;
        put32(&b, size);
        b.insert(b.end(), {'s', 'i', 'd', 'x'});
        put32(&b, static_cast<uint32_t>(version) << 24);
        put32(&b, 1);      // reference_ID
        put32(&b, 1000);   // timescale
        if (version == 0) {
            put32(&b, 500);  // earliest presentation time
            put32(&b, 16);   // first offset
        } else {
            put32(&b, 0), put32(&b, 500), put32(&b, 0), put32(&b, 16);
        }
        b.push_back(0), b.push_back(0), b.push_back(0), b.push_back(3);  // reserved, count
        for (uint32_t k = 0; k < 3; ++k) {
            put32(&b, 1000 + k);  // size
            put32(&b, 2000);      // duration
            put32(&b, 0x90000000u);
        }
        std::vector<MediaSegment> segs;
        std::string err;
        REQUIRE(parse_sidx(b.data(), b.size(), 800, "https://h/v.mp4", &segs, &err));
        REQUIRE(segs.size() == 3);
        CHECK_EQ(segs[0].range.offset, int64_t{800 + size + 16});
        CHECK_EQ(segs[0].range.length, int64_t{1000});
        CHECK_EQ(segs[1].range.offset, int64_t{800 + size + 16 + 1000});
        CHECK_EQ(segs[0].start_us, int64_t{500000});
        CHECK_EQ(segs[2].start_us, int64_t{4500000});
        CHECK_EQ(segs[2].duration_us, int64_t{2000000});
        CHECK_EQ(segs[0].url, std::string("https://h/v.mp4"));
        b[(version == 0 ? 32 : 40)] |= 0x80;  // first reference points at another sidx
        CHECK(!parse_sidx(b.data(), b.size(), 0, "u", &segs, &err));
        CHECK(!parse_sidx(b.data(), 20, 0, "u", &segs, &err));
    }
}

TEST("dash: several Periods of a static MPD continue each other; a dynamic MPD keeps the first") {
    const char *mpd = R"(<MPD type="static" mediaPresentationDuration="PT20S">
      <Period id="main" duration="PT12S">
        <AdaptationSet id="v" contentType="video"><SegmentTemplate media="v1-$Number$" duration="4" startNumber="1"/>
          <Representation id="hi" codecs="avc1.640028" height="1080" bandwidth="5000000"/>
          <Representation id="lo" codecs="avc1.640028" height="720" bandwidth="2000000"/></AdaptationSet>
        <AdaptationSet id="a" contentType="audio" lang="en"><SegmentTemplate media="a1-$Number$" duration="4"/>
          <Representation id="en" codecs="mp4a.40.2" bandwidth="128000"/></AdaptationSet>
      </Period>
      <Period id="credits">
        <AdaptationSet id="x" contentType="video">
          <SegmentTemplate media="v2-$Number$" duration="4" timescale="1" presentationTimeOffset="100"/>
          <Representation id="c720" codecs="avc1.640028" height="720" bandwidth="2100000"/>
          <Representation id="c1080" codecs="avc1.640028" height="1080" bandwidth="4800000"/></AdaptationSet>
        <AdaptationSet id="a" contentType="audio" lang="en"><SegmentTemplate media="a2-$Number$" duration="4"/>
          <Representation id="en" codecs="mp4a.40.2" bandwidth="128000"/></AdaptationSet>
      </Period></MPD>)";
    Presentation p;
    std::string err;
    REQUIRE(parse_mpd(mpd, "https://h/m.mpd", &p, &err));
    CHECK_EQ(p.duration_us, int64_t{20000000});
    REQUIRE(p.renditions.size() == 6);
    auto at = [&](const char *id, int period) {
        for (size_t i = 0; i < p.renditions.size(); ++i) {
            if (p.renditions[i].id == id && p.renditions[i].period == period) return static_cast<int>(i);
        }
        return -1;
    };
    // Ids differ across the video AdaptationSets: the closest bandwidth; the audio keeps its ids.
    CHECK_EQ(p.renditions[static_cast<size_t>(at("hi", 0))].next_period, at("c1080", 1));
    CHECK_EQ(p.renditions[static_cast<size_t>(at("lo", 0))].next_period, at("c720", 1));
    CHECK_EQ(p.renditions[static_cast<size_t>(at("en", 0))].next_period, at("en", 1));
    CHECK_EQ(p.renditions[static_cast<size_t>(at("c720", 1))].next_period, -1);
    const Rendition &c = p.renditions[static_cast<size_t>(at("c720", 1))];
    CHECK_EQ(c.period_start_us, int64_t{12000000});  // the previous Period's start + duration
    CHECK_EQ(c.period_end_us, int64_t{20000000});    // to the presentation's end
    REQUIRE(c.segments.size() == 2);
    CHECK_EQ(c.segments[0].start_us, int64_t{12000000});
    CHECK_EQ(c.segments[1].url, std::string("https://h/v2-2"));
    CHECK_EQ(c.time_origin_us, int64_t{100000000 - 12000000});
    CHECK_EQ(p.renditions[static_cast<size_t>(at("hi", 0))].period_end_us, int64_t{12000000});

    std::string live = mpd;
    live.replace(live.find("static"), 6, "dynamic");
    live.replace(live.find(" mediaPresentationDuration=\"PT20S\""), 34, " availabilityStartTime=\"2026-01-01T00:00:00Z\"");
    REQUIRE(parse_mpd(live, "https://h/m.mpd", &p, &err, 1767225600000000 + 10000000));
    CHECK_EQ(p.renditions.size(), size_t{3});
}

TEST("dash: a later Period's audio is matched by language; a type it lacks ends the chain") {
    const char *mpd = R"(<MPD type="static" mediaPresentationDuration="PT30S">
      <Period id="1" start="PT0S">
        <AdaptationSet contentType="video"><SegmentTemplate media="v-$Number$" duration="5"/>
          <Representation id="v1" codecs="avc1.640028" height="720" bandwidth="2000000"/></AdaptationSet>
        <AdaptationSet contentType="audio" lang="en"><SegmentTemplate media="en-$Number$" duration="5"/>
          <Representation id="a1" codecs="mp4a.40.2" bandwidth="128000"/></AdaptationSet>
        <AdaptationSet contentType="audio" lang="fr"><SegmentTemplate media="fr-$Number$" duration="5"/>
          <Representation id="a2" codecs="mp4a.40.2" bandwidth="96000"/></AdaptationSet>
        <AdaptationSet contentType="text" mimeType="text/vtt" lang="en">
          <Representation id="s1" bandwidth="100"><BaseURL>en.vtt</BaseURL></Representation></AdaptationSet>
      </Period>
      <Period id="2" start="PT10S">
        <AdaptationSet contentType="audio" lang="fr"><SegmentTemplate media="fr2-$Number$" duration="5"/>
          <Representation id="x" codecs="mp4a.40.2" bandwidth="64000"/></AdaptationSet>
        <AdaptationSet contentType="audio" lang="en"><SegmentTemplate media="en2-$Number$" duration="5"/>
          <Representation id="y" codecs="mp4a.40.2" bandwidth="64000"/></AdaptationSet>
        <AdaptationSet contentType="video"><SegmentTemplate media="v2-$Number$" duration="5"/>
          <Representation id="z" codecs="hvc1.1.6.L93.B0" height="720" bandwidth="2000000"/></AdaptationSet>
      </Period></MPD>)";
    Presentation p;
    std::string err;
    REQUIRE(parse_mpd(mpd, "https://h/m.mpd", &p, &err));
    auto find = [&](const char *id) {
        for (size_t i = 0; i < p.renditions.size(); ++i) {
            if (p.renditions[i].id == id) return static_cast<int>(i);
        }
        return -1;
    };
    auto next = [&](const char *id) { return p.renditions[static_cast<size_t>(find(id))].next_period; };
    CHECK_EQ(next("a1"), find("y"));  // same language, not the first audio
    CHECK_EQ(next("a2"), find("x"));
    CHECK_EQ(next("v1"), -1);  // another codec: no continuation, the video ends with its Period
    CHECK_EQ(next("s1"), -1);  // no subtitles in the second Period
    CHECK_EQ(p.renditions[static_cast<size_t>(find("a1"))].period_end_us, int64_t{10000000});  // the next @start
    CHECK_EQ(p.renditions[static_cast<size_t>(find("y"))].period_end_us, int64_t{30000000});
}

namespace {
// An EBML element: ID bytes, a one-byte size (payloads under 127 bytes), the payload.
std::vector<uint8_t> ebml(std::vector<uint8_t> id, const std::vector<uint8_t> &payload) {
    id.push_back(static_cast<uint8_t>(0x80 | payload.size()));
    id.insert(id.end(), payload.begin(), payload.end());
    return id;
}
std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto &p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
std::vector<uint8_t> cue(uint8_t time, uint8_t cluster) {
    return ebml({0xBB}, cat({ebml({0xB3}, {time}), ebml({0xB7}, cat({ebml({0xF7}, {1}), ebml({0xF1}, {cluster})}))}));
}
}  // namespace

TEST("webm: Cues give one segment per Cluster, timed by the TimecodeScale") {
    // EBML header, then a Segment of unknown size whose Info sets 1 ms ticks and a 9 s duration.
    std::vector<uint8_t> init = cat({ebml({0x1A, 0x45, 0xDF, 0xA3}, {}), {0x18, 0x53, 0x80, 0x67, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}});
    const size_t segment_data = init.size();
    // Duration as a 4-byte float: 9000.0f = 0x460CA000.
    init = cat({init, ebml({0x15, 0x49, 0xA9, 0x66}, cat({ebml({0x2A, 0xD7, 0xB1}, {0x0F, 0x42, 0x40}),
                                                         ebml({0x44, 0x89}, {0x46, 0x0C, 0xA0, 0x00})}))});
    // Three Clusters at Segment-relative 100, 50 and 0 (listed out of order), at 60, 30 and 0 ms.
    const std::vector<uint8_t> cues = ebml({0x1C, 0x53, 0xBB, 0x6B}, cat({cue(60, 100), cue(30, 50), cue(0, 0)}));
    std::vector<MediaSegment> segs;
    std::string err;
    REQUIRE(parse_webm_cues(init.data(), init.size(), 0, cues.data(), cues.size(), 5000, "https://h/v.webm", &segs, &err));
    REQUIRE(segs.size() == 3);
    CHECK_EQ(segs[0].range.offset, static_cast<int64_t>(segment_data));
    CHECK_EQ(segs[0].range.length, int64_t{50});
    CHECK_EQ(segs[1].range.offset, static_cast<int64_t>(segment_data + 50));
    CHECK_EQ(segs[2].range.length, static_cast<int64_t>(5000 - (segment_data + 100)));  // up to the Cues
    CHECK_EQ(segs[0].start_us, int64_t{0});
    CHECK_EQ(segs[1].start_us, int64_t{30000});  // 30 ticks of 1 ms
    CHECK_EQ(segs[2].duration_us, int64_t{9000000 - 60000});
    CHECK_EQ(segs[1].url, std::string("https://h/v.webm"));
    // Cues before the Clusters: the last one runs to the end of the file.
    REQUIRE(parse_webm_cues(init.data(), init.size(), 0, cues.data(), cues.size(), 10, "u", &segs, &err));
    CHECK_EQ(segs[2].range.length, int64_t{-1});
    CHECK(!parse_webm_cues(init.data(), 4, 0, cues.data(), cues.size(), 0, "u", &segs, &err));    // no Segment
    CHECK(!parse_webm_cues(init.data(), init.size(), 0, init.data(), init.size(), 0, "u", &segs, &err));  // no Cues
}

// ---- live ----------------------------------------------------------------------------------------

TEST("ISO 8601 date-times: UTC, fractions, zones") {
    int64_t us = 0;
    REQUIRE(parse_iso_datetime("1970-01-01T00:00:00Z", &us));
    CHECK_EQ(us, int64_t{0});
    REQUIRE(parse_iso_datetime("2026-10-12T08:30:00.250Z", &us));
    CHECK_EQ(us, int64_t{1791793800250000});
    REQUIRE(parse_iso_datetime("2026-10-12T10:30:00.250+02:00", &us));
    CHECK_EQ(us, int64_t{1791793800250000});
    REQUIRE(parse_iso_datetime("2000-02-29T00:00:00", &us));  // no zone: UTC
    CHECK_EQ(us, int64_t{951782400000000});
    CHECK(!parse_iso_datetime("2026-13-01T00:00:00Z", &us));
    CHECK(!parse_iso_datetime("yesterday", &us));
}

TEST("dash live: an endless SegmentTemplate lists what has arrived, back to the time-shift window") {
    // Shaped like DASH-IF livesim2's testpic_2s.
    const char *mpd = R"(<MPD type="dynamic" availabilityStartTime="1970-01-01T00:00:00Z" minimumUpdatePeriod="PT2S"
        timeShiftBufferDepth="PT1M" suggestedPresentationDelay="PT6S"><Period id="P0" start="PT0S">
      <AdaptationSet contentType="video" mimeType="video/mp4">
        <SegmentTemplate media="$RepresentationID$/$Number$.m4s" initialization="$RepresentationID$/init.mp4" duration="2" startNumber="0"/>
        <Representation id="V300" bandwidth="300000" width="640" height="360" codecs="avc1.64001e"/>
      </AdaptationSet></Period>
      <UTCTiming schemeIdUri="urn:mpeg:dash:utc:http-xsdate:2014" value="https://time.akamai.com/?iso"/></MPD>)";
    Presentation p;
    std::string err;
    CHECK(!parse_mpd(mpd, "https://h/l/Manifest.mpd", &p, &err));  // no clock
    const int64_t now = int64_t{1791793801} * 1000000 + 500000;    // ...:01.5
    REQUIRE(parse_mpd(mpd, "https://h/l/Manifest.mpd", &p, &err, now));
    CHECK(p.live);
    CHECK_EQ(p.duration_us, kNoTime);
    CHECK_EQ(p.update_period_us, int64_t{2000000});
    CHECK_EQ(p.time_shift_us, int64_t{60000000});
    CHECK_EQ(p.live_delay_us, int64_t{6000000});
    CHECK_EQ(p.availability_start_us, int64_t{0});
    CHECK_EQ(p.utc_timing_scheme, std::string("urn:mpeg:dash:utc:http-xsdate:2014"));
    const auto &segs = p.renditions[0].segments;
    REQUIRE(segs.size() == 30);  // 60 s of 2 s segments
    // 1791793801.5 s elapsed: segment 895896899 (starting 1791793798) is the last complete one.
    CHECK_EQ(segs.back().number, int64_t{895896899});
    CHECK_EQ(segs.back().start_us, int64_t{1791793798000000});
    CHECK_EQ(segs.back().url, std::string("https://h/l/V300/895896899.m4s"));
    CHECK_EQ(segs.front().number, int64_t{895896870});
}

TEST("dash live: a SegmentTimeline ending in r=-1 repeats up to now") {
    const char *mpd = R"(<MPD type="dynamic" availabilityStartTime="2026-10-12T08:00:00Z"><Period start="PT0S">
      <AdaptationSet contentType="video"><SegmentTemplate timescale="1000" media="$Time$.m4s">
        <SegmentTimeline><S t="0" d="4000" r="-1"/></SegmentTimeline></SegmentTemplate>
        <Representation id="v" bandwidth="1" codecs="avc1.64001f"/></AdaptationSet></Period></MPD>)";
    int64_t ast = 0;
    REQUIRE(parse_iso_datetime("2026-10-12T08:00:00Z", &ast));
    Presentation p;
    std::string err;
    REQUIRE(parse_mpd(mpd, "https://h/m.mpd", &p, &err, ast + 20500000));
    const auto &segs = p.renditions[0].segments;
    REQUIRE(segs.size() == 5);  // 0 ... 16 s: the 20 s one is still arriving at 20.5 s
    CHECK_EQ(segs[4].url, std::string("https://h/16000.m4s"));
    CHECK_EQ(segs[4].number, int64_t{5});
}

TEST("hls live: media sequence numbers and the target duration are kept") {
    Rendition r;
    bool live = false;
    std::string err;
    REQUIRE(parse_hls_media("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:1234\n#EXTINF:6,\na.ts\n#EXTINF:5.5,\nb.ts\n",
                            "https://h/live.m3u8", &r, &live, &err));
    CHECK(live);
    CHECK_EQ(r.target_duration_us, int64_t{6000000});
    REQUIRE(r.segments.size() == 2);
    CHECK_EQ(r.segments[0].number, int64_t{1234});
    CHECK_EQ(r.segments[1].number, int64_t{1235});
}
