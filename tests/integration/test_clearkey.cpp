// SPDX-License-Identifier: GPL-3.0-or-later
//
// ClearKey (wnk/media/clearkey.hpp): key parsing, the W3C licence exchange against the local test
// server's /clearkey route, and decryption checked byte for byte. The system ffmpeg CLI writes a clear
// MP4 and an encrypted copy ('cenc' AES-CTR); read with the key, the copy must give exactly the clear
// file's packets. Run under tests/integration/http_server.py.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/adaptive_source.hpp"
#include "wnk/media/clearkey.hpp"
#include "wnk/media/ffmpeg_source.hpp"
#include "wnk/net/http.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace wnk;
using namespace wnk::media;

namespace {

const std::string kKid = "0123456789abcdef0123456789abcdef";  // known to http_server.py's /clearkey
const std::string kKey = "00112233445566778899aabbccddeeff";
const std::string kWrongKey = "000102030405060708090a0b0c0d0e0f";

std::string env(const char *name) {
    const char *v = std::getenv(name);
    return v != nullptr ? v : "";
}

bool have_server() { return !env("WNK_TEST_HTTP_BASE").empty() && !env("WNK_TEST_FILES_DIR").empty(); }

// clear.mp4, enc.mp4 (the same packets, encrypted) and e.mpd (encrypted DASH from the same packets,
// with ContentProtection naming the key ID and the local licence route).
bool make_media() {
    static const bool ok = [] {
        if (std::system("ffmpeg -hide_banner -version >/dev/null 2>&1") != 0) return false;
        const std::string d = env("WNK_TEST_FILES_DIR");
        const std::string enc = "encryption_scheme=cenc-aes-ctr:encryption_key=" + kKey + ":encryption_kid=" + kKid;
        const std::string cmd =
            "cd '" + d + "' && ffmpeg -hide_banner -loglevel error -y "
            "-f lavfi -i testsrc2=size=320x240:rate=25:duration=6 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=6 "
            "-map 0:v -map 1:a -c:v libx264 -threads 1 -g 25 -keyint_min 25 -sc_threshold 0 -c:a aac clear.mp4 && "
            "ffmpeg -hide_banner -loglevel error -y -i clear.mp4 -c copy -encryption_scheme cenc-aes-ctr "
            "-encryption_key " + kKey + " -encryption_kid " + kKid + " enc.mp4 && "
            "ffmpeg -hide_banner -loglevel error -y -i clear.mp4 -map 0:v -map 0:a -c copy -f dash -seg_duration 2 "
            "-format_options '" + enc + "' -init_seg_name 'e-init-$RepresentationID$.m4s' "
            "-media_seg_name 'e-$RepresentationID$-$Number%05d$.m4s' e.mpd";
        if (std::system(cmd.c_str()) != 0) return false;
        // ffmpeg does not declare the encryption: add what a packager would.
        std::ifstream in(d + "/e.mpd");
        std::stringstream ss;
        ss << in.rdbuf();
        std::string mpd = ss.str();
        const std::string cp =
            "<ContentProtection xmlns:cenc=\"urn:mpeg:cenc:2013\" schemeIdUri=\"urn:mpeg:dash:mp4protection:2011\" value=\"cenc\" "
            "cenc:default_KID=\"01234567-89ab-cdef-0123-456789abcdef\"/>"
            "<ContentProtection schemeIdUri=\"urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e\" value=\"ClearKey1.0\">"
            "<dashif:Laurl xmlns:dashif=\"https://dashif.org/CPS\">" + env("WNK_TEST_HTTP_BASE") + "/clearkey</dashif:Laurl>"
            "</ContentProtection>";
        for (size_t at = mpd.find("<AdaptationSet"); at != std::string::npos; at = mpd.find("<AdaptationSet", at + 1)) {
            const size_t end = mpd.find('>', at);
            mpd.insert(end + 1, cp);
        }
        std::ofstream(d + "/e.mpd") << mpd;
        // The same presentation with a licence URL that refuses.
        std::string denied = mpd;
        for (size_t at = denied.find("/clearkey<"); at != std::string::npos; at = denied.find("/clearkey<", at + 1)) {
            denied.replace(at, 10, "/clearkey/deny<");
        }
        std::ofstream(d + "/denied.mpd") << denied;
        return true;
    }();
    return ok;
}

std::shared_ptr<net::HttpTransport> transport() {
    static std::shared_ptr<net::HttpTransport> t = [] {
        net::CurlOptions o;
        o.ca_bundle = env("WNK_TEST_CA");
        return std::shared_ptr<net::HttpTransport>(net::make_curl_transport(o));
    }();
    return t;
}

BackendCaps passthrough() {
    BackendCaps c;
    c.video_codecs = {CodecId::H264};
    c.audio_codecs = {CodecId::Aac};
    return c;
}

struct Read {
    std::vector<std::vector<uint8_t>> video, audio;
    std::string error;
};

Read read_all(MediaSource &s, size_t max_video = 1000) {
    Read out;
    Packet p;
    std::string err;
    MediaSource::Read r;
    while (out.video.size() < max_video && (r = s.read(&p, &err)) == MediaSource::Read::Packet) {
        if (p.type == TrackType::Video) out.video.push_back(p.data);
        if (p.type == TrackType::Audio) out.audio.push_back(p.data);
    }
    if (out.video.size() < max_video && r == MediaSource::Read::Error) out.error = err;
    return out;
}

Read read_file(const std::string &path, const ContentKeys &keys) {
    FfmpegSourceOptions o;
    o.decryption_keys = keys;
    FfmpegSource s(o);
    std::string err;
    Read fail;
    if (!s.open("file://" + path, &err)) {
        fail.error = "open: " + err;
        return fail;
    }
    StreamFormat f;
    if (!s.select(s.info().best(TrackType::Video)->index, s.info().best(TrackType::Audio)->index, passthrough(), &f, &err)) {
        fail.error = "select: " + err;
        return fail;
    }
    return read_all(s);
}

#define NEEDS_MEDIA()                                                  \
    if (!have_server() || !make_media()) {                             \
        std::printf("  SKIP: needs http_server.py and the ffmpeg CLI\n"); \
        return;                                                        \
    }

}  // namespace

TEST("keys: kid:key lists, UUID and base64url spellings, JWK sets; malformed input adds nothing") {
    ContentKeys k;
    std::string err;
    REQUIRE(parse_content_keys("01234567-89AB-cdef-0123-456789abcdef:00112233445566778899AABBCCDDEEFF, "
                               "feedf00deedeadbeeff0baadf00dd00d=000102030405060708090a0b0c0d0e0f",
                               &k, &err));
    CHECK_EQ(k.size(), 2u);
    CHECK_EQ(k[kKid], kKey);
    CHECK_EQ(normalize_key_id("KID-NuRL-b950nUuI0gYpQ"), std::string("2880fe36e44bf9bf79d2752e234818a5"));
    CHECK_EQ(normalize_key_id("0123"), std::string());
    CHECK_EQ(normalize_key_id("zz23456789abcdef0123456789abcdef"), std::string());
    ContentKeys j;
    REQUIRE(parse_content_keys(R"({"keys":[{"kty":"oct","kid":"KID-NuRL-b950nUuI0gYpQ","k":"KEY-NuRL-b950nUuI0gYpQ"}]})", &j, &err));
    CHECK_EQ(j.size(), 1u);
    CHECK(j.count("2880fe36e44bf9bf79d2752e234818a5") == 1u);
    ContentKeys bad;
    CHECK(!parse_content_keys(kKid + ":" + kKey + ",nonsense", &bad, &err));
    CHECK(bad.empty());
    CHECK(err.find("entry 2") != std::string::npos);
    CHECK(err.find(kKey) == std::string::npos);  // errors never repeat a key
    CHECK(!parse_content_keys("", &bad, &err));
    CHECK(!parse_content_keys(R"({"keys":[{"kid":"x"}]})", &bad, &err));
    CHECK_EQ(ffmpeg_decryption_keys(k), "0123456789abcdef0123456789abcdef=00112233445566778899aabbccddeeff:"
                                        "feedf00deedeadbeeff0baadf00dd00d=000102030405060708090a0b0c0d0e0f");
}

TEST("licence: W3C request body; the local server's answer, refusals and missing keys") {
    CHECK_EQ(clearkey_license_request({kKid}), std::string(R"({"kids":["ASNFZ4mrze8BI0VniavN7w"],"type":"temporary"})"));
    if (!have_server()) {
        std::printf("  SKIP: needs http_server.py\n");
        return;
    }
    const std::string base = env("WNK_TEST_HTTP_BASE");
    ContentKeys k;
    std::string err;
    REQUIRE(fetch_clearkey_license(*transport(), base + "/clearkey", {}, {kKid}, {}, &k, &err));
    CHECK_EQ(k[kKid], kKey);
    ContentKeys none;
    CHECK(!fetch_clearkey_license(*transport(), base + "/clearkey/deny", {}, {kKid}, {}, &none, &err));
    CHECK(err.find("403") != std::string::npos);
    CHECK(!fetch_clearkey_license(*transport(), base + "/clearkey/empty", {}, {kKid}, {}, &none, &err));
    CHECK(err.find("none of the 1 requested") != std::string::npos);
    CHECK(!fetch_clearkey_license(*transport(), base + "/clearkey", {}, {"ffffffffffffffffffffffffffffffff"}, {}, &none, &err));
    CHECK(none.empty());
}

TEST("manifest: ContentProtection gives the scheme, default KIDs and the ClearKey licence URL") {
    const std::string mpd = R"(<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT4S">
      <Period><AdaptationSet contentType="video" mimeType="video/mp4">
        <ContentProtection xmlns:cenc="urn:mpeg:cenc:2013" schemeIdUri="urn:mpeg:dash:mp4protection:2011" value="cbcs"
                           cenc:default_KID="2880FE36-e44b-f9bf-79d2-752e234818a5"/>
        <ContentProtection schemeIdUri="urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e" value="ClearKey1.0">
          <clearkey:Laurl xmlns:clearkey="http://dashif.org/guidelines/clearKey" Lic_type="EME-1.0">lic/ck</clearkey:Laurl>
        </ContentProtection>
        <SegmentTemplate media="$Number$.m4s" initialization="init.mp4" duration="2" startNumber="1"/>
        <Representation id="v" bandwidth="1000" codecs="avc1.64001e" width="640" height="360"/>
      </AdaptationSet></Period></MPD>)";
    Presentation p;
    std::string err;
    REQUIRE(parse_mpd(mpd, "https://cdn.example/live/x.mpd", &p, &err, 0));
    REQUIRE(p.renditions.size() == 1u);
    const Rendition &r = p.renditions[0];
    CHECK(r.encrypted);
    CHECK_EQ(r.protection_scheme, std::string("cbcs"));
    REQUIRE(r.key_ids.size() == 1u);
    CHECK_EQ(r.key_ids[0], std::string("2880fe36e44bf9bf79d2752e234818a5"));
    CHECK_EQ(r.license_url, std::string("https://cdn.example/live/lic/ck"));
}

TEST("mp4: with the key every packet equals the clear file's; a wrong key differs; no key fails clearly") {
    NEEDS_MEDIA();
    const std::string d = env("WNK_TEST_FILES_DIR");
    const Read clear = read_file(d + "/clear.mp4", {});
    REQUIRE(clear.error.empty());
    REQUIRE(clear.video.size() == 150u);  // 6 s at 25 fps
    const Read good = read_file(d + "/enc.mp4", {{kKid, kKey}});
    CHECK(good.error.empty());
    CHECK(good.video == clear.video);
    CHECK(good.audio == clear.audio);
    const Read wrong = read_file(d + "/enc.mp4", {{kKid, kWrongKey}});
    REQUIRE(wrong.video.size() == clear.video.size());
    CHECK(wrong.video != clear.video);
    const Read none = read_file(d + "/enc.mp4", {});
    CHECK(none.video.empty());
    CHECK(none.error.find("encrypted and no decryption key") != std::string::npos);
    const Read other = read_file(d + "/enc.mp4", {{"ffffffffffffffffffffffffffffffff", kKey}});
    CHECK(!other.error.empty());  // the key ID in the file has no key
}

// ffmpeg's fragmented CENC output (dash muxer, frag_keyframe) does not demux ("Incorrect number of
// samples in encryption info", "saio atom found without saiz"), so the DASH tests stop at what is
// DASH's own: KIDs from the manifest, the licence exchange and which renditions become playable.
// Sample decryption is the MP4 test above; real fragmented 'cenc' and 'cbcs' streams are checked
// with Example D (Shaka angel-one-clearkey, livesim2 eccp_cenc / eccp_cbcs).
TEST("dash: the manifest's licence server supplies the key; the encrypted renditions become playable") {
    NEEDS_MEDIA();
    AdaptiveSourceOptions o;
    o.http = transport();
    o.video_codecs = {CodecId::H264};
    AdaptiveSource s(o);
    std::string err;
    REQUIRE(s.open(env("WNK_TEST_HTTP_BASE") + "/files/e.mpd", &err));
    CHECK(s.presentation().renditions[0].encrypted);
    StreamFormat f;
    CHECK(s.select(0, 1, passthrough(), &f, &err));
    CHECK(f.video.has_value());
    CHECK(f.audio.has_value());
}

TEST("dash: given keys are used without asking; a refusing licence server leaves the video unplayable") {
    NEEDS_MEDIA();
    AdaptiveSourceOptions o;
    o.http = transport();
    o.video_codecs = {CodecId::H264};
    o.keys = {{kKid, kKey}};
    {
        AdaptiveSource s(o);
        std::string err;
        REQUIRE(s.open(env("WNK_TEST_HTTP_BASE") + "/files/denied.mpd", &err));  // never asked
    }
    o.keys.clear();
    AdaptiveSource s(o);
    std::string err;
    CHECK(!s.open(env("WNK_TEST_HTTP_BASE") + "/files/denied.mpd", &err));
    CHECK(err.find("encrypted") != std::string::npos);
    CHECK(err.find("403") != std::string::npos);
    o.fetch_licenses = false;
    AdaptiveSource s2(o);
    CHECK(!s2.open(env("WNK_TEST_HTTP_BASE") + "/files/e.mpd", &err));
    CHECK(err.find("no key for key ID " + kKid) != std::string::npos);
}
