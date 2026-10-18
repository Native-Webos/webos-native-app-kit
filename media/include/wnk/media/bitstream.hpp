// SPDX-License-Identifier: GPL-3.0-or-later
//
// Elementary-stream rewrites the sources apply before packets reach a hardware decoder. Pure byte
// manipulation, no FFmpeg, so each one is unit-tested on its own.
//
//   * HDR10+ (SMPTE ST 2094-40) SEI messages are removed from HEVC: PlxNative (#822) measured a
//     webOS decoder stalling on them and Kodi removes them on every webOS TV. The OLED65C1PTB (webOS
//     6.5.3) plays them either way (records: media.hevc.hdr10plus), so this guards other sets. Other messages
//     in the same SEI NAL unit (mastering display, content light level) stay, because the pipeline
//     builds the panel's HDR10 envelope from them.
//   * AV1 samples from MP4/Matroska become complete temporal units: a temporal delimiter first and,
//     on keyframes, the sequence header from the av1C record (PlxNative #602 feeds AV1 this way).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wnk::media {

// Removes HDR10+ messages from the prefix SEI NAL units of one HEVC Annex-B access unit (those come
// before the first slice), rebuilding a NAL unit that holds other messages too and dropping one left
// empty. A NAL unit that does not parse is left as it is. Returns true if `au` changed.
bool hevc_strip_hdr10plus(std::vector<uint8_t> *au);

// One AV1 temporal unit (low-overhead OBU format) from one demuxed sample. `av1c` is the stream's
// AV1CodecConfigurationRecord (extradata): its config OBUs (after the 4-byte header) go in front of a
// keyframe that carries no sequence header of its own.
std::vector<uint8_t> av1_temporal_unit(const uint8_t *data, size_t size, const std::vector<uint8_t> &av1c, bool key);

}  // namespace wnk::media
