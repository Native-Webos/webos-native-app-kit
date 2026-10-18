// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/bitstream.hpp"

using namespace wnk::media;
using Bytes = std::vector<uint8_t>;

namespace {

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes &p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// One SEI message: payloadType, payloadSize (both < 255 here), payload.
Bytes message(uint8_t type, const Bytes &payload) {
    return cat({{type, static_cast<uint8_t>(payload.size())}, payload});
}

const Bytes kHdr10Plus = {0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04, 0x01, 0x40, 0x00, 0x0C, 0x80};
const Bytes kMastering(24, 0x11);
const Bytes kLight = {0x03, 0xE8, 0x01, 0x90};  // MaxCLL 1000, MaxFALL 400
const Bytes kSeiHeader = {0x4E, 0x01};          // prefix SEI, layer 0, tid 1
const Bytes kSlice = {0x00, 0x00, 0x01, 0x26, 0x01, 0xAF, 0x09, 0x40};  // IDR_W_RADL

}  // namespace

TEST("hevc: an HDR10+ message is removed and the mastering display beside it stays") {
    Bytes au = cat({{0, 0, 0, 1}, kSeiHeader, message(137, kMastering), message(4, kHdr10Plus), message(144, kLight), {0x80}, kSlice});
    CHECK(hevc_strip_hdr10plus(&au));
    CHECK(au == cat({{0, 0, 0, 1}, kSeiHeader, message(137, kMastering), message(144, kLight), {0x80}, kSlice}));
}

TEST("hevc: an SEI holding only HDR10+ is dropped with its start code") {
    const Bytes aud = {0, 0, 0, 1, 0x46, 0x01, 0x10};
    Bytes au = cat({aud, {0, 0, 0, 1}, kSeiHeader, message(4, kHdr10Plus), {0x80}, kSlice});
    CHECK(hevc_strip_hdr10plus(&au));
    CHECK(au == cat({aud, kSlice}));
}

TEST("hevc: emulation prevention survives the rebuild") {
    // A kept payload whose RBSP has 00 00 01 needs 00 00 03 01 on the wire, before and after.
    const Bytes wire_payload = {0x00, 0x00, 0x03, 0x01, 0x22};
    Bytes au = cat({{0, 0, 1}, kSeiHeader, {5, 4}, wire_payload, message(4, kHdr10Plus), {0x80}, kSlice});
    CHECK(hevc_strip_hdr10plus(&au));
    CHECK(au == cat({{0, 0, 1}, kSeiHeader, {5, 4}, wire_payload, {0x80}, kSlice}));
}

TEST("hevc: access units without HDR10+, malformed SEIs and SEIs after a slice are left alone") {
    Bytes plain = cat({{0, 0, 0, 1}, kSeiHeader, message(137, kMastering), {0x80}, kSlice});
    const Bytes plain_copy = plain;
    CHECK(!hevc_strip_hdr10plus(&plain));
    CHECK(plain == plain_copy);

    Bytes bad = cat({{0, 0, 1}, kSeiHeader, {4, 60}, kHdr10Plus, {0x80}, kSlice});  // size runs past the NAL
    const Bytes bad_copy = bad;
    CHECK(!hevc_strip_hdr10plus(&bad));
    CHECK(bad == bad_copy);

    Bytes late = cat({kSlice, {0, 0, 1}, kSeiHeader, message(4, kHdr10Plus), {0x80}});
    const Bytes late_copy = late;
    CHECK(!hevc_strip_hdr10plus(&late));
    CHECK(late == late_copy);
}

namespace {
const Bytes kTd = {0x12, 0x00};
const Bytes kSeqHeader = {0x0A, 0x03, 0x00, 0x00, 0x00};  // OBU_SEQUENCE_HEADER, size 3
const Bytes kFrame = {0x32, 0x02, 0xAA, 0xBB};             // OBU_FRAME, size 2
const Bytes kAv1c = cat({{0x81, 0x00, 0x0C, 0x00}, kSeqHeader});
}  // namespace

TEST("av1: a keyframe gets a temporal delimiter and the av1C sequence header") {
    CHECK(av1_temporal_unit(kFrame.data(), kFrame.size(), kAv1c, true) == cat({kTd, kSeqHeader, kFrame}));
    CHECK(av1_temporal_unit(kFrame.data(), kFrame.size(), kAv1c, false) == cat({kTd, kFrame}));
}

TEST("av1: a sample's own delimiter stays first and its own sequence header is not doubled") {
    const Bytes with_td = cat({kTd, kFrame});
    CHECK(av1_temporal_unit(with_td.data(), with_td.size(), kAv1c, true) == cat({kTd, kSeqHeader, kFrame}));
    const Bytes with_seq = cat({kSeqHeader, kFrame});
    CHECK(av1_temporal_unit(with_seq.data(), with_seq.size(), kAv1c, true) == cat({kTd, kSeqHeader, kFrame}));
    CHECK(av1_temporal_unit(kFrame.data(), kFrame.size(), {}, true) == cat({kTd, kFrame}));
}
