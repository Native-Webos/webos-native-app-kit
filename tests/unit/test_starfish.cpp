// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/starfish_backend.hpp"

using namespace wnk::media;

namespace {
bool has(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }
}  // namespace

TEST("starfish: frame-rate classes and fractions") {
    CHECK_EQ(StarfishBackend::frame_rate_class(23.976), 24);
    CHECK_EQ(StarfishBackend::frame_rate_class(24), 24);
    CHECK_EQ(StarfishBackend::frame_rate_class(25), 25);
    CHECK_EQ(StarfishBackend::frame_rate_class(29.97), 30);
    CHECK_EQ(StarfishBackend::frame_rate_class(50), 50);
    CHECK_EQ(StarfishBackend::frame_rate_class(59.94), 60);
    CHECK_EQ(StarfishBackend::frame_rate_class(120), 120);
    int64_t n = 0, d = 0;
    StarfishBackend::frame_rate_fraction(23.976, &n, &d);
    CHECK(n == 24000 && d == 1001);
    StarfishBackend::frame_rate_fraction(25, &n, &d);
    CHECK(n == 25 && d == 1);
    StarfishBackend::frame_rate_fraction(12.5, &n, &d);
    CHECK(n == 12500 && d == 1000);
}

TEST("starfish: codec names follow LG's vocabulary") {
    CHECK_EQ(std::string(StarfishBackend::codec_string(CodecId::H264)), std::string("H264"));
    CHECK_EQ(std::string(StarfishBackend::codec_string(CodecId::Hevc)), std::string("H265"));
    CHECK_EQ(std::string(StarfishBackend::codec_string(CodecId::Eac3)), std::string("AC3 PLUS"));
    CHECK(StarfishBackend::codec_string(CodecId::Mp3) == nullptr);
    CHECK(StarfishBackend::codec_string(CodecId::PcmS16) == nullptr);
}

TEST("starfish: Load payload for 1080p24 H.264 + AAC declares FHD at 24 with the window and fps") {
    StarfishLoad l;
    l.app_id = "org.wnk.examples.player";
    l.window_id = "_Window_Id_3";
    l.video = CodecId::H264;
    l.width = 1920;
    l.height = 1080;
    l.frame_rate = 23.976;
    l.audio = CodecId::Aac;
    const std::string p = StarfishBackend::build_load_payload(l);
    CHECK(has(p, "\"mediaTransportType\":\"BUFFERSTREAM\""));
    CHECK(has(p, "\"appId\":\"org.wnk.examples.player\",\"windowId\":\"_Window_Id_3\""));
    CHECK(has(p, "\"codec\":{\"video\":\"H264\",\"audio\":\"AAC\"}"));
    CHECK(has(p, "\"videoFpsValue\":24000,\"videoFpsScale\":1001"));
    CHECK(has(p, "\"maxWidth\":1920,\"maxHeight\":1080,\"maxFrameRate\":24"));
    CHECK(has(p, "\"needAudio\":true"));
    CHECK(has(p, "\"lowDelayMode\":false"));
    // Balanced braces and brackets: the payload is one JSON document.
    int depth = 0;
    for (char c : p) {
        if (c == '{' || c == '[') ++depth;
        if (c == '}' || c == ']') --depth;
        CHECK(depth >= 0);
    }
    CHECK_EQ(depth, 0);
}

TEST("starfish: HEVC declares 4K at 60; video-only drops audio and uses low delay; 1088 counts as FHD") {
    StarfishLoad l;
    l.app_id = "a";
    l.video = CodecId::Hevc;
    l.width = 1920;
    l.height = 1080;
    l.frame_rate = 24;
    std::string p = StarfishBackend::build_load_payload(l);
    CHECK(has(p, "\"maxWidth\":3840,\"maxHeight\":2160,\"maxFrameRate\":60"));
    CHECK(has(p, "\"codec\":{\"video\":\"H265\"}"));
    CHECK(has(p, "\"needAudio\":false"));
    CHECK(has(p, "\"lowDelayMode\":true"));
    CHECK(!has(p, "windowId"));
    l.video = CodecId::H264;
    l.height = 1088;
    l.frame_rate = 0;  // unknown: no fps fields, rate ceiling 60
    p = StarfishBackend::build_load_payload(l);
    CHECK(has(p, "\"maxWidth\":1920,\"maxHeight\":1080,\"maxFrameRate\":60"));
    CHECK(!has(p, "videoFpsValue"));
}

TEST("starfish: Dolby Vision and Atmos nodes go into contents, profileId as an integer, codec still H265") {
    StarfishLoad l;
    l.app_id = "a";
    l.video = CodecId::Hevc;
    l.width = 3840;
    l.height = 2160;
    l.frame_rate = 59.94;
    l.audio = CodecId::Eac3;
    std::string p = StarfishBackend::build_load_payload(l);
    CHECK(!has(p, "DolbyHdrInfo"));
    CHECK(!has(p, "immersive"));
    l.dolby_vision_profile = 8;
    l.atmos = true;
    p = StarfishBackend::build_load_payload(l);
    CHECK(has(p, "\"codec\":{\"video\":\"H265\",\"audio\":\"AC3 PLUS\"}"));
    CHECK(has(p, "\"provider\":\"wnk\",\"DolbyHdrInfo\":{\"trackType\":\"single\",\"encryptionType\":\"clear\",\"profileId\":8},\"immersive\":\"ATMOS\"}"));
    int depth = 0;
    for (char c : p) {
        if (c == '{' || c == '[') ++depth;
        if (c == '}' || c == ']') --depth;
        CHECK(depth >= 0);
    }
    CHECK_EQ(depth, 0);
    // Only where they mean something: DV on HEVC, Atmos on E-AC-3.
    l.video = CodecId::H264;
    l.audio = CodecId::Aac;
    p = StarfishBackend::build_load_payload(l);
    CHECK(!has(p, "DolbyHdrInfo"));
    CHECK(!has(p, "immersive"));
}

TEST("starfish: unavailable on the host, with a reason; caps") {
    std::string why;
    CHECK(!StarfishBackend::available(&why));
    CHECK(!why.empty());
    StarfishBackend b;
    StreamFormat f;
    f.video = VideoFormat{CodecId::H264, 1280, 720, 25};
    std::string err;
    CHECK(!b.load(f, &err));
    CHECK(err.find("Starfish unavailable") != std::string::npos);
    const BackendCaps c = b.caps();
    CHECK(c.pause && c.position && c.seek_splices && c.aac_adts);
    CHECK(!c.pause_drains);
    CHECK(c.accepts(CodecId::Aac) && !c.accepts(CodecId::PcmS16));
}

TEST("starfish: VP9 and AV1 names; only answers above the 1920x1088 default name a decoder") {
    CHECK_EQ(std::string(StarfishBackend::codec_string(CodecId::Vp9)), std::string("VP9"));
    CHECK_EQ(std::string(StarfishBackend::codec_string(CodecId::Av1)), std::string("AV1"));
    // OLED65C1PTB answers: VP9/AV1 4096x2304, an unknown name 1920x1088.
    CHECK(StarfishBackend::decoder_listed(4096, 2304));
    CHECK(!StarfishBackend::decoder_listed(1920, 1088));
}

TEST("starfish: the TV's decoder size bounds the envelope; E-AC-3 declares ac3PlusInfo") {
    StarfishLoad l;
    l.video = CodecId::Vp9;
    l.width = 1920;
    l.height = 1080;
    l.frame_rate = 30;
    l.audio = CodecId::Eac3;
    l.audio_channels = 6;
    l.audio_rate = 48000;
    std::string p = StarfishBackend::build_load_payload(l);
    CHECK(has(p, "\"video\":\"VP9\""));
    CHECK(has(p, "\"maxWidth\":3840,\"maxHeight\":2160"));
    CHECK(has(p, "\"ac3PlusInfo\":{\"channels\":6,\"frequency\":48}"));
    l.atmos = true;
    CHECK(has(StarfishBackend::build_load_payload(l), "\"ac3PlusInfo\":{\"channels\":8,\"frequency\":48}"));
    l.decoder_max_width = 1920;
    l.decoder_max_height = 1088;
    CHECK(has(StarfishBackend::build_load_payload(l), "\"maxWidth\":1920,\"maxHeight\":1088"));
    l.audio = CodecId::Ac3;
    CHECK(!has(StarfishBackend::build_load_payload(l), "ac3PlusInfo"));
}

TEST("starfish: setHdrInfo payload from container HDR10 metadata, in SEI units and G,B,R order") {
    VideoColour c;
    CHECK(StarfishBackend::build_hdr_payload(c).empty());  // SDR
    c.transfer = 16;
    c.primaries = 9;
    c.matrix = 9;
    CHECK(StarfishBackend::build_hdr_payload(c).empty());  // PQ without metadata: nothing to declare
    c.has_primaries = true;
    c.display_primaries[0][0] = 0.708, c.display_primaries[0][1] = 0.292;  // R
    c.display_primaries[1][0] = 0.170, c.display_primaries[1][1] = 0.797;  // G
    c.display_primaries[2][0] = 0.131, c.display_primaries[2][1] = 0.046;  // B
    c.white_point[0] = 0.3127, c.white_point[1] = 0.3290;
    c.has_luminance = true;
    c.min_luminance = 0.0001, c.max_luminance = 1000;
    c.has_light_level = true;
    c.max_cll = 1000, c.max_fall = 400;
    CHECK_EQ(StarfishBackend::build_hdr_payload(c),
             std::string(R"({"hdrType":"HDR10","sei":{"displayPrimariesX0":8500,"displayPrimariesY0":39850,)"
                         R"("displayPrimariesX1":6550,"displayPrimariesY1":2300,"displayPrimariesX2":35400,"displayPrimariesY2":14600,)"
                         R"("whitePointX":15635,"whitePointY":16450,"minDisplayMasteringLuminance":1,)"
                         R"("maxDisplayMasteringLuminance":10000000,"maxContentLightLevel":1000,"maxPicAverageLightLevel":400},)"
                         R"("vui":{"transferCharacteristics":16,"colorPrimaries":9,"matrixCoeffs":9,"videoFullRangeFlag":false}})"));
    c.transfer = 18;
    CHECK(has(StarfishBackend::build_hdr_payload(c), "\"hdrType\":\"HLG\""));
}
