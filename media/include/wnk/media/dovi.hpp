// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dolby Vision Profile 7 (a dual layer: an HDR10-compatible base layer, an enhancement layer and
// RPUs, the shape of every UHD Blu-ray remux) played as Profile 8.1, as `dovi_tool -m 2` converts it
// and PlxNative does on its feed path (its #825): every enhancement-layer NAL unit is dropped (the
// UNSPEC63 wrapper a single-track dual layer stores them in, and anything with nuh_layer_id > 0) and
// every RPU (UNSPEC62) is rewritten for 8.1 by quietvoid's libdovi. An RPU that does not parse or
// convert is dropped, never passed on as Profile 7. What is lost is the enhancement layer's
// residual detail above the base layer.
//
// Without libdovi in the build (WNK_DOVI off), dovi_available() is false: the stream still loses
// its enhancement layer and RPUs and plays as its HDR10 base layer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wnk::media {

// A configuration record (dvcC/dvvC) whose stream converts: Profile 7 with an enhancement layer
// and a base layer cross-compatible with HDR10 (bl_signal_compatibility_id 6).
bool dovi_converts_to_p81(int profile, bool el_present, int bl_compatibility_id);

// Whether this build has libdovi.
bool dovi_available();

// One RPU NAL unit (HEVC UNSPEC62 from its 0x7C01 header, emulation-prevention bytes included)
// rewritten for Profile 8.1, in the same form. False with *error when it does not parse or convert.
bool dovi_rpu_to_p81(const uint8_t *nal, size_t size, std::vector<uint8_t> *out, std::string *error);

struct DoviStats {
    uint64_t el_dropped = 0;     // enhancement-layer NAL units removed
    uint64_t rpu_converted = 0;  // RPUs rewritten
    uint64_t rpu_dropped = 0;    // RPUs removed because the rewrite failed
    std::string last_error;      // of the last failed rewrite
};

using RpuRewrite = std::function<bool(const uint8_t *nal, size_t size, std::vector<uint8_t> *out, std::string *error)>;

// An HEVC access unit (Annex B) without its enhancement-layer NAL units and with every RPU passed
// through `rewrite` (dropped when it fails; a null rewrite drops them all). True when the unit changed.
bool hevc_dovi_p7_to_p81(std::vector<uint8_t> *au, const RpuRewrite &rewrite, DoviStats *stats);

}  // namespace wnk::media
