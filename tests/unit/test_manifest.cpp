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
