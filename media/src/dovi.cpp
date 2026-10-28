// SPDX-License-Identifier: GPL-3.0-or-later
//
// Profile 7 -> 8.1 on the feed path, following PlxNative's packet_to_annexb (Copyright the PlxNative
// authors, GPL-3.0-or-later). The RPU rewrite is libdovi's (quietvoid, MIT): parse_unspec62_nalu,
// convert_rpu_with_mode(2) and write_unspec62_nalu, as `dovi_tool -m 2`.
#include "wnk/media/dovi.hpp"

#if WNK_HAVE_DOVI
#include "wnk_dovi.h"
#endif

namespace wnk::media {
namespace {

constexpr int kHevcUnspec62 = 62;  // Dolby Vision RPU
constexpr int kHevcUnspec63 = 63;  // Dolby Vision enhancement layer (single-track dual layer)

// Start of the next Annex-B start code (00 00 01) at or after `from`, or `size`.
size_t next_start_code(const uint8_t *d, size_t size, size_t from) {
    for (size_t i = from; i + 3 <= size; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return i;
    }
    return size;
}

void append_nal(std::vector<uint8_t> *out, const uint8_t *nal, size_t size) {
    static constexpr uint8_t kStart[4] = {0, 0, 0, 1};
    out->insert(out->end(), kStart, kStart + 4);
    out->insert(out->end(), nal, nal + size);
}

}  // namespace

bool dovi_converts_to_p81(int profile, bool el_present, int bl_compatibility_id) {
    return profile == 7 && el_present && bl_compatibility_id == 6;
}

bool dovi_available() { return WNK_HAVE_DOVI != 0; }

bool dovi_rpu_to_p81(const uint8_t *nal, size_t size, std::vector<uint8_t> *out, std::string *error) {
#if WNK_HAVE_DOVI
    DoviRpuOpaque *rpu = dovi_parse_unspec62_nalu(nal, size);
    if (rpu == nullptr) {
        *error = "dovi_parse_unspec62_nalu returned nothing";
        return false;
    }
    bool ok = false;
    if (const char *e = dovi_rpu_get_error(rpu)) {
        *error = std::string("parse: ") + e;
    } else if (dovi_convert_rpu_with_mode(rpu, 2) != 0) {
        const char *e2 = dovi_rpu_get_error(rpu);
        *error = std::string("convert: ") + (e2 != nullptr ? e2 : "failed");
    } else if (const DoviData *data = dovi_write_unspec62_nalu(rpu)) {
        if (const char *e3 = dovi_rpu_get_error(rpu)) {
            *error = std::string("write: ") + e3;
        } else {
            out->assign(data->data, data->data + data->len);
            ok = true;
        }
        dovi_data_free(data);
    } else {
        *error = "dovi_write_unspec62_nalu returned nothing";
    }
    dovi_rpu_free(rpu);
    return ok;
#else
    (void)nal;
    (void)size;
    (void)out;
    *error = "built without libdovi";
    return false;
#endif
}

bool hevc_dovi_p7_to_p81(std::vector<uint8_t> *au, const RpuRewrite &rewrite, DoviStats *stats) {
    const uint8_t *d = au->data();
    const size_t size = au->size();
    std::vector<uint8_t> out;
    out.reserve(size);
    bool changed = false;
    size_t sc = next_start_code(d, size, 0);
    while (sc < size) {
        const size_t nal = sc + 3;
        const size_t next = next_start_code(d, size, nal);
        size_t nal_end = next;
        // A four-byte start code's leading zero belongs to the next NAL unit's start code.
        while (nal_end > nal && d[nal_end - 1] == 0 && next < size) --nal_end;
        if (nal + 2 > nal_end) {
            sc = next;
            continue;
        }
        const int type = (d[nal] >> 1) & 0x3F;
        const int layer = ((d[nal] & 1) << 5) | (d[nal + 1] >> 3);
        if (type == kHevcUnspec63 || layer > 0) {
            ++stats->el_dropped;
            changed = true;
        } else if (type == kHevcUnspec62) {
            std::vector<uint8_t> rpu;
            std::string err;
            if (rewrite && rewrite(d + nal, nal_end - nal, &rpu, &err)) {
                append_nal(&out, rpu.data(), rpu.size());
                ++stats->rpu_converted;
            } else {
                ++stats->rpu_dropped;
                stats->last_error = rewrite ? err : "no rewrite";
            }
            changed = true;
        } else {
            append_nal(&out, d + nal, nal_end - nal);
        }
        sc = next;
    }
    if (!changed) return false;
    *au = std::move(out);
    return true;
}

}  // namespace wnk::media
