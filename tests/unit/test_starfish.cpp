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
    CHECK_EQ(std::string(StarfishBackend::codec_string(CodecId::PcmS16)), std::string("PCM"));
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
    CHECK(c.accepts(CodecId::Aac) && c.accepts(CodecId::PcmS16));
    CHECK_EQ(c.pcm_max_channels, 2);  // stereo PCM; wider tracks go to AC-3
    CHECK(c.pcm_rates.size() == 8 && c.pcm_rates.back() == 48000);
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

namespace {
VideoColour hdr10_colour() {
    VideoColour c;
    c.transfer = 16;
    c.primaries = 9;
    c.matrix = 9;
    c.has_primaries = true;
    c.display_primaries[0][0] = 0.708, c.display_primaries[0][1] = 0.292;  // R
    c.display_primaries[1][0] = 0.170, c.display_primaries[1][1] = 0.797;  // G
    c.display_primaries[2][0] = 0.131, c.display_primaries[2][1] = 0.046;  // B
    c.white_point[0] = 0.3127, c.white_point[1] = 0.3290;
    c.has_luminance = true;
    c.min_luminance = 0.0001, c.max_luminance = 1000;
    c.has_light_level = true;
    c.max_cll = 1000, c.max_fall = 400;
    return c;
}
}  // namespace

TEST("starfish: setHdrInfo before webOS 5 names its objects mediaSei and mediaVui") {
    const std::string p = StarfishBackend::build_hdr_payload(hdr10_colour(), true);
    CHECK(has(p, "\"hdrType\":\"HDR10\",\"mediaSei\":{\"displayPrimariesX0\":8500"));
    CHECK(has(p, "\"mediaVui\":{\"transferCharacteristics\":16"));
    CHECK(!has(p, "\"sei\""));
    CHECK(!has(p, "\"vui\""));
}

TEST("starfish: webOS major from os_info.json") {
    CHECK_EQ(StarfishBackend::webos_major_from_os_info(R"({"webos_release":"4.4.2","core_os_release":"4.4.2-22"})"), 4);
    CHECK_EQ(StarfishBackend::webos_major_from_os_info(R"({"webos_release":"3.9.3"})"), 3);
    CHECK_EQ(StarfishBackend::webos_major_from_os_info(R"({"webos_release":"10.2.0"})"), 10);
    CHECK_EQ(StarfishBackend::webos_major_from_os_info(R"({"core_os_release":"6.5.3-47"})"), 0);
    CHECK_EQ(StarfishBackend::webos_major_from_os_info(R"({"webos_release":"x"})"), 0);
    CHECK_EQ(StarfishBackend::webos_major_from_os_info("not json"), 0);
}

TEST("starfish: event types above ENDOFSTREAM are two lower before webOS 5") {
    // webOS 4.10's libpf sends dropped/displayed frames as 46/47 (PlxNative's decompile).
    CHECK_EQ(StarfishBackend::normalise_event_type(46, 4), 48);
    CHECK_EQ(StarfishBackend::normalise_event_type(47, 3), 49);
    CHECK_EQ(StarfishBackend::normalise_event_type(48, 6), 48);
    CHECK_EQ(StarfishBackend::normalise_event_type(48, 0), 48);  // unknown: webOS 5+ numbering
    // At and below ENDOFSTREAM (0x1c) nothing moves.
    CHECK_EQ(StarfishBackend::normalise_event_type(0x1c, 4), 0x1c);
    CHECK_EQ(StarfishBackend::normalise_event_type(0x16, 4), 0x16);
    CHECK_EQ(StarfishBackend::normalise_event_type(0, 4), 0);
}

TEST("starfish: a PQ stream's SDR video-info envelope is rewritten to HDR10 for ACB") {
    // The envelope a webOS 4.5 TV produced for a VP9 PQ file (PlxNative's vp9_hdr.rs fixture).
    const std::string vp9 =
        R"({"context":"_x","content":"movie","video":{"frameRate":0,"width":1920,"height":1080,"hdrType":"none",)"
        R"("mediaVui":{"transferCharacteristics":2,"colorPrimaries":2,"matrixCoeffs":2}}})";
    const std::string out = StarfishBackend::rewrite_video_info(vp9, hdr10_colour());
    CHECK(has(out, "\"hdrType\":\"HDR10\""));
    CHECK(has(out, "\"transferCharacteristics\":16"));
    CHECK(has(out, "\"displayPrimariesX0\":8500"));  // G first
    CHECK(has(out, "\"maxContentLightLevel\":1000"));
    CHECK(has(out, "\"context\":\"_x\""));  // the rest stays
    CHECK(has(out, "\"width\":1920"));
    // The TV's own word wins; SDR content, other shapes and junk are left alone.
    const std::string hevc = R"({"video":{"hdrType":"HDR10","mediaVui":{"transferCharacteristics":16}}})";
    CHECK(StarfishBackend::rewrite_video_info(hevc, hdr10_colour()).empty());
    VideoColour sdr;
    CHECK(StarfishBackend::rewrite_video_info(vp9, sdr).empty());
    CHECK(StarfishBackend::rewrite_video_info(R"({"audio":{}})", hdr10_colour()).empty());
    CHECK(StarfishBackend::rewrite_video_info("garbage", hdr10_colour()).empty());
    // Without mastering metadata: HDR10 with the VUI only, and no stale SEI.
    VideoColour bare;
    bare.transfer = 16, bare.primaries = 9, bare.matrix = 9;
    const std::string b = StarfishBackend::rewrite_video_info(R"({"video":{"mediaSei":{"x":1}}})", bare);
    CHECK(has(b, "\"hdrType\":\"HDR10\""));
    CHECK(!has(b, "mediaSei"));
}

TEST("starfish: decoded PCM is declared with pcmInfo, its rate in kHz") {
    StarfishLoad l;
    l.app_id = "a";
    l.video = CodecId::H264;
    l.audio = CodecId::PcmS16;
    l.audio_channels = 2;
    l.audio_rate = 44100;
    const std::string p = StarfishBackend::build_load_payload(l);
    CHECK(has(p, "\"codec\":{\"video\":\"H264\",\"audio\":\"PCM\"}"));
    CHECK(has(p, "\"pcmInfo\":{\"format\":\"S16LE\",\"channelMode\":\"stereo\",\"layout\":\"interleaved\","
                 "\"sampleRate\":44.1,\"bitsPerSample\":16}"));
    l.audio_channels = 1;
    l.audio_rate = 48000;
    CHECK(has(StarfishBackend::build_load_payload(l), "\"channelMode\":\"mono\",\"layout\":\"interleaved\",\"sampleRate\":48,"));
    l.audio = CodecId::Aac;
    CHECK(!has(StarfishBackend::build_load_payload(l), "pcmInfo"));
}

TEST("starfish: the PCM lead follows the audio stack and the sound output") {
    CHECK_EQ(StarfishBackend::pcm_lead_ms_for(AudioStack::Liblxa, "tv_speaker"), 400);
    CHECK_EQ(StarfishBackend::pcm_lead_ms_for(AudioStack::Liblxa, "bt_soundbar"), 250);
    CHECK_EQ(StarfishBackend::pcm_lead_ms_for(AudioStack::Liblxa, "external_arc"), 400);
    CHECK_EQ(StarfishBackend::pcm_lead_ms_for(AudioStack::Liblxa, ""), 400);
    CHECK_EQ(StarfishBackend::pcm_lead_ms_for(AudioStack::AudioSinkNew, "bt_soundbar"), 250);
    CHECK_EQ(StarfishBackend::pcm_lead_ms_for(AudioStack::LgAudioSink, "bt_soundbar"), 400);
    CHECK_EQ(StarfishBackend::pcm_lead_ms_for(AudioStack::AdecSink, "tv_speaker"), 0);
    CHECK_EQ(StarfishBackend::pcm_lead_ms_for(AudioStack::Other, "tv_speaker"), 0);
    CHECK_EQ(StarfishBackend::sound_output_from_reply(
                 R"({"category":"sound","method":"getSystemSettings","settings":{"soundOutput":"tv_speaker"},"returnValue":true})"),
             std::string("tv_speaker"));
    CHECK_EQ(StarfishBackend::sound_output_from_reply(R"({"returnValue":false})"), std::string());
    CHECK_EQ(StarfishBackend::sound_output_from_reply("junk"), std::string());
}
