// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/dovi.hpp"

#include <fstream>
#include <iterator>

using namespace wnk::media;

namespace {
// An HEVC NAL unit header: forbidden 0, type, nuh_layer_id, temporal id + 1 = 1.
std::vector<uint8_t> nal(int type, int layer, std::vector<uint8_t> payload) {
    std::vector<uint8_t> n = {static_cast<uint8_t>((type << 1) | (layer >> 5)), static_cast<uint8_t>(((layer & 31) << 3) | 1)};
    n.insert(n.end(), payload.begin(), payload.end());
    return n;
}
std::vector<uint8_t> au(const std::vector<std::vector<uint8_t>> &nals, bool short_codes = false) {
    std::vector<uint8_t> out;
    for (const auto &n : nals) {
        if (!short_codes) out.push_back(0);
        out.insert(out.end(), {0, 0, 1});
        out.insert(out.end(), n.begin(), n.end());
    }
    return out;
}
std::vector<uint8_t> fixture(const char *name) {
    std::ifstream f(std::string(WNK_FIXTURES_DIR) + "/dovi/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
// A fixture (start code + RPU payload) as the UNSPEC62 NAL unit a stream carries: 0x7C01 + payload.
std::vector<uint8_t> rpu_nal(const std::vector<uint8_t> &bin) {
    std::vector<uint8_t> n = {0x7C, 0x01};
    n.insert(n.end(), bin.begin() + 4, bin.end());
    return n;
}
}  // namespace

TEST("dovi: only Profile 7 with an enhancement layer and an HDR10-compatible base layer converts") {
    CHECK(dovi_converts_to_p81(7, true, 6));
    CHECK(!dovi_converts_to_p81(7, true, 0));   // no cross-compatible base layer
    CHECK(!dovi_converts_to_p81(7, false, 6));  // no enhancement layer: nothing to convert
    CHECK(!dovi_converts_to_p81(8, false, 1));
    CHECK(!dovi_converts_to_p81(5, false, 0));
}

TEST("dovi: enhancement-layer units leave, RPUs are rewritten, the rest stays in order") {
    const auto vps = nal(32, 0, {1, 2, 3});
    const auto slice = nal(1, 0, {9, 9, 9, 9});
    const auto el = nal(63, 0, {7, 7});
    const auto el_layer1 = nal(1, 1, {6, 6});
    const auto rpu = nal(62, 0, {0x19, 5, 5});
    std::vector<uint8_t> unit = au({vps, slice, el, el_layer1, rpu});
    DoviStats st;
    auto rewrite = [](const uint8_t *d, size_t n, std::vector<uint8_t> *out, std::string *) {
        out->assign(d, d + n);
        out->push_back(0xAA);  // marks it rewritten
        return true;
    };
    CHECK(hevc_dovi_p7_to_p81(&unit, rewrite, &st));
    auto rewritten = rpu;
    rewritten.push_back(0xAA);
    CHECK(unit == au({vps, slice, rewritten}));
    CHECK_EQ(st.el_dropped, 2u);
    CHECK_EQ(st.rpu_converted, 1u);
    CHECK_EQ(st.rpu_dropped, 0u);

    // Three-byte start codes and a trailing zero of a four-byte one are handled the same.
    unit = au({vps, el, slice}, true);
    CHECK(hevc_dovi_p7_to_p81(&unit, rewrite, &st));
    CHECK(unit == au({vps, slice}));
}

TEST("dovi: a failed or missing rewrite drops the RPU; an access unit with neither is untouched") {
    const auto slice = nal(1, 0, {9, 9});
    const auto rpu = nal(62, 0, {0x19});
    std::vector<uint8_t> unit = au({slice, rpu});
    DoviStats st;
    auto fail = [](const uint8_t *, size_t, std::vector<uint8_t> *, std::string *e) {
        *e = "bad rpu";
        return false;
    };
    CHECK(hevc_dovi_p7_to_p81(&unit, fail, &st));
    CHECK(unit == au({slice}));
    CHECK_EQ(st.rpu_dropped, 1u);
    CHECK_EQ(st.last_error, std::string("bad rpu"));
    unit = au({slice, rpu});
    CHECK(hevc_dovi_p7_to_p81(&unit, RpuRewrite(), &st));
    CHECK(unit == au({slice}));
    const std::vector<uint8_t> plain = au({nal(32, 0, {1}), slice});
    unit = plain;
    CHECK(!hevc_dovi_p7_to_p81(&unit, fail, &st));
    CHECK(unit == plain);
}

TEST("dovi: libdovi rewrites dovi_tool's FEL and MEL RPUs to its own Profile 8.1 output, byte for byte") {
    if (!dovi_available()) {
        std::printf("  (built without libdovi: skipped)\n");
        return;
    }
    struct Case {
        const char *from, *to;
    };
    for (const Case c : {Case{"fel_to_mel.bin", "fel_to_81.bin"}, Case{"mel_orig.bin", "mel_to_81.bin"}}) {
        const auto in = fixture(c.from), want = fixture(c.to);
        REQUIRE(in.size() > 8 && want.size() > 8);
        std::vector<uint8_t> out;
        std::string err;
        const auto n = rpu_nal(in);
        CHECK(dovi_rpu_to_p81(n.data(), n.size(), &out, &err));
        CHECK_EQ(err, std::string());
        CHECK(out == rpu_nal(want));
    }
    // A FEL RPU straight to 8.1 (as dovi_tool -m 2 on a FEL stream; upstream's fel_to_81.bin went
    // through MEL first and differs), inside an access unit with its enhancement layer.
    const auto fel = rpu_nal(fixture("fel_orig.bin"));
    std::vector<uint8_t> direct;
    std::string err;
    REQUIRE(dovi_rpu_to_p81(fel.data(), fel.size(), &direct, &err));
    CHECK(direct != fel);
    std::vector<uint8_t> again;  // a converted RPU converts to itself
    CHECK(dovi_rpu_to_p81(direct.data(), direct.size(), &again, &err));
    CHECK(again == direct);
    std::vector<uint8_t> unit = au({nal(1, 0, {9, 9}), nal(63, 0, {7}), fel});
    DoviStats st;
    CHECK(hevc_dovi_p7_to_p81(&unit, &dovi_rpu_to_p81, &st));
    CHECK_EQ(st.rpu_converted, 1u);
    CHECK_EQ(st.el_dropped, 1u);
    CHECK(unit == au({nal(1, 0, {9, 9}), direct}));
}

TEST("dovi: libdovi refuses a damaged RPU with a reason") {
    if (!dovi_available()) return;
    const std::vector<uint8_t> junk = {0x7C, 0x01, 0x19, 0x08, 0x00, 0x00};
    std::vector<uint8_t> out;
    std::string err;
    CHECK(!dovi_rpu_to_p81(junk.data(), junk.size(), &out, &err));
    CHECK(!err.empty());
}
