// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/bitstream.hpp"

namespace wnk::media {
namespace {

constexpr int kHevcPrefixSei = 39;
constexpr int kHevcFirstNonVcl = 32;  // NAL unit types below this are slices

// Start of the next Annex-B start code (00 00 01) at or after `from`, or `size`.
size_t next_start_code(const uint8_t *d, size_t size, size_t from) {
    for (size_t i = from; i + 2 < size; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return i;
    }
    return size;
}

// RBSP from a NAL unit payload: emulation-prevention bytes (00 00 03) removed.
std::vector<uint8_t> unescape(const uint8_t *d, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; ++i) {
        if (zeros >= 2 && d[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = d[i] == 0 ? zeros + 1 : 0;
        out.push_back(d[i]);
    }
    return out;
}

void escape_into(const std::vector<uint8_t> &rbsp, std::vector<uint8_t> *out) {
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out->push_back(3);
            zeros = 0;
        }
        zeros = b == 0 ? zeros + 1 : 0;
        out->push_back(b);
    }
}

bool is_hdr10plus(int type, const uint8_t *p, size_t n) {
    // user_data_registered_itu_t_t35: country B5 (USA), provider 003C (Samsung), oriented code 0001,
    // application identifier 4 (ST 2094-40).
    return type == 4 && n >= 6 && p[0] == 0xB5 && p[1] == 0x00 && p[2] == 0x3C && p[3] == 0x00 && p[4] == 0x01 &&
           p[5] == 0x04;
}

enum class Sei { Unchanged, Rebuilt, Empty };

// Removes HDR10+ messages from one SEI RBSP (after the 2-byte NAL header). `kept` gets the remaining
// messages followed by the trailing bits.
Sei strip_sei(const std::vector<uint8_t> &rbsp, std::vector<uint8_t> *kept) {
    size_t i = 0;
    bool removed = false, any_kept = false;
    std::vector<uint8_t> out;
    // A message starts where more than the trailing bits (0x80, then zero padding) remain.
    auto more = [&] {
        size_t end = rbsp.size();
        while (end > i && rbsp[end - 1] == 0) --end;
        return end > i + 1 || (end == i + 1 && rbsp[i] != 0x80);
    };
    while (i < rbsp.size() && more()) {
        const size_t begin = i;
        int type = 0, size = 0;
        while (i < rbsp.size() && rbsp[i] == 0xFF) type += rbsp[i++];
        if (i >= rbsp.size()) return Sei::Unchanged;
        type += rbsp[i++];
        while (i < rbsp.size() && rbsp[i] == 0xFF) size += rbsp[i++];
        if (i >= rbsp.size()) return Sei::Unchanged;
        size += rbsp[i++];
        if (i + static_cast<size_t>(size) > rbsp.size()) return Sei::Unchanged;
        if (is_hdr10plus(type, rbsp.data() + i, static_cast<size_t>(size))) {
            removed = true;
        } else {
            out.insert(out.end(), rbsp.begin() + static_cast<ptrdiff_t>(begin),
                       rbsp.begin() + static_cast<ptrdiff_t>(i + size));
            any_kept = true;
        }
        i += static_cast<size_t>(size);
    }
    if (!removed) return Sei::Unchanged;
    if (!any_kept) return Sei::Empty;
    out.push_back(0x80);
    *kept = std::move(out);
    return Sei::Rebuilt;
}

// AV1 OBU types.
constexpr int kObuSequenceHeader = 1;
constexpr int kObuTemporalDelimiter = 2;

// Calls fn(type, end) for each OBU in [d, d+n), `end` being the offset just past it; false if the
// OBUs do not parse.
template <typename Fn>
bool each_obu(const uint8_t *d, size_t n, Fn fn) {
    size_t i = 0;
    while (i < n) {
        const uint8_t h = d[i];
        const int type = (h >> 3) & 0x0F;
        const bool ext = (h & 0x04) != 0, has_size = (h & 0x02) != 0;
        i += 1 + (ext ? 1 : 0);
        if (i > n) return false;
        if (!has_size) {
            fn(type, n);
            return true;  // runs to the end
        }
        uint64_t len = 0;
        for (int b = 0;; ++b) {
            if (i >= n || b >= 8) return false;
            const uint8_t v = d[i++];
            len |= static_cast<uint64_t>(v & 0x7F) << (7 * b);
            if ((v & 0x80) == 0) break;
        }
        if (len > n - i) return false;
        i += static_cast<size_t>(len);
        fn(type, i);
    }
    return true;
}

}  // namespace

bool hevc_strip_hdr10plus(std::vector<uint8_t> *au) {
    const uint8_t *d = au->data();
    const size_t size = au->size();
    std::vector<uint8_t> out;
    bool changed = false;
    size_t copied = 0;  // bytes of `au` already accounted for in `out`
    size_t sc = next_start_code(d, size, 0);
    while (sc < size) {
        const size_t nal = sc + 3;
        size_t next = next_start_code(d, size, nal);
        size_t nal_end = next;
        // A four-byte start code's leading zero belongs to the next NAL unit's start code.
        while (nal_end > nal && d[nal_end - 1] == 0 && next < size) --nal_end;
        if (nal + 2 > nal_end) {
            sc = next;
            continue;
        }
        const int type = (d[nal] >> 1) & 0x3F;
        if (type < kHevcFirstNonVcl) break;  // prefix SEIs precede the first slice
        if (type == kHevcPrefixSei) {
            std::vector<uint8_t> kept;
            const Sei r = strip_sei(unescape(d + nal + 2, nal_end - nal - 2), &kept);
            if (r != Sei::Unchanged) {
                // Everything up to this NAL unit's start code (with its leading zero), then the rebuilt unit.
                size_t sc_begin = sc;
                if (sc_begin > copied && d[sc_begin - 1] == 0) --sc_begin;
                out.insert(out.end(), d + copied, d + sc_begin);
                if (r == Sei::Rebuilt) {
                    out.insert(out.end(), d + sc_begin, d + nal + 2);
                    escape_into(kept, &out);
                }
                copied = nal_end;
                changed = true;
            }
        }
        sc = next;
    }
    if (!changed) return false;
    out.insert(out.end(), d + copied, d + size);
    *au = std::move(out);
    return true;
}

std::vector<uint8_t> av1_temporal_unit(const uint8_t *data, size_t size, const std::vector<uint8_t> &av1c, bool key) {
    size_t td_end = 0;  // the sample's own temporal delimiter, if it starts with one
    bool has_sequence_header = false, first = true;
    const bool parsed = each_obu(data, size, [&](int type, size_t end) {
        if (first && type == kObuTemporalDelimiter) td_end = end;
        first = false;
        if (type == kObuSequenceHeader) has_sequence_header = true;
    });
    std::vector<uint8_t> out;
    out.reserve(size + 2 + av1c.size());
    if (td_end > 0) {
        out.insert(out.end(), data, data + td_end);
    } else {
        out.push_back(static_cast<uint8_t>(kObuTemporalDelimiter << 3 | 0x02));  // header with has_size
        out.push_back(0);
    }
    // av1C: marker+version 0x81, then three bytes of profile/level/flags, then config OBUs.
    if (parsed && key && !has_sequence_header && av1c.size() > 4 && av1c[0] == 0x81) {
        out.insert(out.end(), av1c.begin() + 4, av1c.end());
    }
    out.insert(out.end(), data + td_end, data + size);
    return out;
}

}  // namespace wnk::media
