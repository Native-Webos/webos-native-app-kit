// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/adaptive_source.hpp"
#include "wnk/media/clearkey.hpp"

#include "wnk/log.hpp"
#include "wnk/media/ffmpeg_source.hpp"
#include "wnk/media/ttml.hpp"
#include "wnk/media/webvtt.hpp"

extern "C" {
#include <libavutil/aes.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace wnk::media {
namespace {

constexpr const char *kCat = "media.abr";

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string &s, const char *suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// A video rendition's codec; HLS variants without CODECS are taken as H.264 (the HLS default).
CodecId video_codec(const Rendition &r) {
    if (r.codecs.empty()) return CodecId::H264;
    return codec_from_rfc6381(r.codecs, TrackType::Video);
}

// The entry of an RFC 6381 list that is of `type` ("avc1.64001f" from "mp4a.40.2,avc1.64001f").
std::string codec_entry(const std::string &codecs, TrackType type) {
    size_t i = 0;
    while (i <= codecs.size()) {
        size_t comma = codecs.find(',', i);
        if (comma == std::string::npos) comma = codecs.size();
        const std::string c = codecs.substr(i, comma - i);
        if (codec_from_rfc6381(c, type) != CodecId::Unknown) return c;
        i = comma + 1;
    }
    return codecs.substr(0, codecs.find(','));
}

// WebM renditions index their segments with Cues, not a sidx; not read yet.
bool is_webm(const Rendition &r) { return lower(r.mime_type).find("webm") != std::string::npos; }

// "en-US" matches a preference of "en" and "en-us".
bool language_matches(const std::string &have, const std::string &want) {
    if (want.empty() || have.empty()) return false;
    const std::string h = lower(have), w = lower(want);
    return h == w || (h.size() > w.size() && h.compare(0, w.size(), w) == 0 && h[w.size()] == '-') ||
           (w.size() > h.size() && w.compare(0, h.size(), h) == 0 && w[h.size()] == '-');
}

std::string mbps(int64_t bps) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f Mb/s", static_cast<double>(bps) / 1e6);
    return buf;
}

// Index of the last segment starting at or before `t` (0 if none does).
size_t segment_at(const std::vector<MediaSegment> &segs, int64_t t) {
    auto it = std::upper_bound(segs.begin(), segs.end(), t,
                               [](int64_t v, const MediaSegment &s) { return v < s.start_us; });
    return it == segs.begin() ? 0 : static_cast<size_t>(it - segs.begin()) - 1;
}


// ---- HLS EXT-X-KEY ------------------------------------------------------------------------------

// HLS keys by URI, fetched once each (a playlist can rotate keys per segment). Shared with the
// download threads that ask for them, so it outlives any reader.
struct HlsKeyCache {
    std::shared_ptr<net::HttpTransport> http;
    net::Headers headers;
    std::mutex m;
    std::map<std::string, std::string> keys;  // URI → 16 key bytes

    bool get(const std::string &uri, std::string *key, std::string *error) {
        {
            std::lock_guard<std::mutex> lock(m);
            const auto it = keys.find(uri);
            if (it != keys.end()) {
                *key = it->second;
                return true;
            }
        }
        if (uri.rfind("skd://", 0) == 0) {
            *error = "FairPlay keys (skd://) are not supported";
            return false;
        }
        if (!http || uri.empty()) {
            *error = "no way to fetch the key";
            return false;
        }
        net::HttpRequest req;
        req.url = uri;
        req.headers = headers;
        req.timeout_ms = 10000;
        req.max_response_bytes = 4096;
        net::HttpResponse r;
        for (int attempt = 0; attempt < 3; ++attempt) {
            r = http->perform(req, {});
            if (r.error == net::NetError::None && r.status < 500) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(300 << attempt));
        }
        if (r.error != net::NetError::None || r.status < 200 || r.status >= 300) {
            *error = "key request failed: " + (r.error != net::NetError::None ? std::string(net::error_name(r.error))
                                                                             : "HTTP " + std::to_string(r.status));
            return false;
        }
        std::string bytes = r.body;
        if (bytes.size() != 16) {
            // Some servers answer the key as 32 hex digits.
            std::string hex = bytes;
            while (!hex.empty() && std::isspace(static_cast<unsigned char>(hex.back()))) hex.pop_back();
            const std::string norm = normalize_key_id(hex);
            if (norm.size() != 32 || hex.size() != 32) {
                *error = "the key is " + std::to_string(bytes.size()) + " bytes, not 16";
                return false;
            }
            bytes.clear();
            for (size_t i = 0; i < 32; i += 2) bytes.push_back(static_cast<char>(std::stoi(norm.substr(i, 2), nullptr, 16)));
        }
        std::lock_guard<std::mutex> lock(m);
        keys[uri] = bytes;
        *key = bytes;
        return true;
    }
};

// The IV of a segment: EXT-X-KEY's IV, else its media sequence number as a 128-bit big-endian value.
void hls_iv(const HlsKey &k, int64_t number, uint8_t iv[16]) {
    std::memset(iv, 0, 16);
    if (k.iv.size() == 32) {
        for (int i = 0; i < 16; ++i) iv[i] = static_cast<uint8_t>(std::stoi(k.iv.substr(static_cast<size_t>(i) * 2, 2), nullptr, 16));
        return;
    }
    const uint64_t n = static_cast<uint64_t>(std::max<int64_t>(0, number));
    for (int i = 0; i < 8; ++i) iv[15 - i] = static_cast<uint8_t>(n >> (8 * i));
}

// AES-128-CBC over a whole segment with PKCS#7 padding (HLS METHOD=AES-128), streamed: the last
// block is held back until the end so the padding can be removed.
class Aes128CbcFilter final : public net::PartFilter {
 public:
    Aes128CbcFilter(const std::string &key, const uint8_t iv[16]) : aes_(av_aes_alloc()) {
        av_aes_init(aes_, reinterpret_cast<const uint8_t *>(key.data()), 128, 1);
        std::memcpy(iv_, iv, 16);
    }
    ~Aes128CbcFilter() override { av_free(aes_); }

    bool update(const uint8_t *data, size_t n, std::string *out, std::string *) override {
        pending_.append(reinterpret_cast<const char *>(data), n);
        size_t blocks = pending_.size() / 16;
        if (blocks > 0 && pending_.size() % 16 == 0) --blocks;  // may be the padded last block
        decrypt(blocks, out);
        return true;
    }
    bool finish(std::string *out, std::string *error) override {
        if (pending_.empty()) return true;
        if (pending_.size() != 16) {
            *error = "AES-128 segment is not a whole number of blocks";
            return false;
        }
        const size_t before = out->size();
        decrypt(1, out);
        const uint8_t pad = static_cast<uint8_t>(out->back());
        bool padded = pad >= 1 && pad <= 16 && out->size() - before >= pad;
        for (size_t i = 0; padded && i < pad; ++i) padded = static_cast<uint8_t>((*out)[out->size() - 1 - i]) == pad;
        if (padded) out->resize(out->size() - pad);  // without valid padding the block is kept whole
        return true;
    }

 private:
    void decrypt(size_t blocks, std::string *out) {
        if (blocks == 0) return;
        const size_t at = out->size();
        out->resize(at + blocks * 16);
        av_aes_crypt(aes_, reinterpret_cast<uint8_t *>(&(*out)[at]), reinterpret_cast<const uint8_t *>(pending_.data()),
                     static_cast<int>(blocks), iv_, 1);
        pending_.erase(0, blocks * 16);
    }
    AVAES *aes_;
    uint8_t iv_[16];
    std::string pending_;
};

}  // namespace

void ThroughputEstimator::add(int64_t bytes, double seconds) {
    if (bytes < (16 << 10) || seconds <= 0) return;
    const double bps = static_cast<double>(bytes) * 8 / seconds;
    std::lock_guard<std::mutex> lock(m_);
    // Each sample weighs by its transfer time: alpha = 1 - 0.5^(seconds / half-life).
    const double af = 1 - std::pow(0.5, seconds / 3.0), as = 1 - std::pow(0.5, seconds / 10.0);
    fast_ = af * bps + (1 - af) * fast_;
    slow_ = as * bps + (1 - as) * slow_;
    fast_weight_ = af + (1 - af) * fast_weight_;  // how much of the average is real (bias correction)
    slow_weight_ = as + (1 - as) * slow_weight_;
    bytes_ += bytes;
    ++samples_;
}

int64_t ThroughputEstimator::estimate_bps() const {
    std::lock_guard<std::mutex> lock(m_);
    if (bytes_ < (128 << 10) || fast_weight_ <= 0 || slow_weight_ <= 0) return 0;
    return static_cast<int64_t>(std::min(fast_ / fast_weight_, slow_ / slow_weight_));
}

int ThroughputEstimator::samples() const {
    std::lock_guard<std::mutex> lock(m_);
    return samples_;
}

int choose_rung(const std::vector<int64_t> &bw, int current, int64_t estimate, int64_t limit) {
    const int n = static_cast<int>(bw.size());
    if (n == 0) return -1;
    current = std::clamp(current, 0, n - 1);
    auto highest_within = [&](double budget) {
        int best = 0;
        for (int i = 0; i < n; ++i) {
            if (static_cast<double>(bw[static_cast<size_t>(i)]) <= budget) best = i;
        }
        return best;
    };
    const double cap = limit > 0 ? static_cast<double>(limit) : 1e300;
    int target = current;
    if (estimate > 0) {
        const double up = std::min(0.70 * static_cast<double>(estimate), cap);
        const double stay = std::min(0.85 * static_cast<double>(estimate), cap);
        if (static_cast<double>(bw[static_cast<size_t>(current)]) > stay) {
            target = highest_within(stay);
        } else {
            target = std::max(current, highest_within(up));
        }
    }
    if (static_cast<double>(bw[static_cast<size_t>(target)]) > cap) target = highest_within(cap);
    return target;
}

bool decryptable(const Rendition &r, const ContentKeys &keys) {
    if (!r.encrypted) return true;
    // HLS EXT-X-KEY: the key comes from its URI (identity keys only; not FairPlay).
    for (const MediaSegment &seg : r.segments) {
        if (seg.key) return seg.key->key_format == "identity";
    }
    if (keys.empty()) return false;
    // Without default KIDs in the manifest the keys are tried; the demuxer reports a missing one.
    return std::all_of(r.key_ids.begin(), r.key_ids.end(), [&](const std::string &k) { return keys.count(k) != 0; });
}

int choose_video(const Presentation &p, const AdaptiveSourceOptions &o) {
    bool any_plain = false;
    for (const Rendition &r : p.renditions) {
        if (r.type == TrackType::Video && r.period == 0 && decryptable(r, o.keys) && r.dolby_vision.empty()) any_plain = true;
    }
    int best = -1;
    auto rank = [&](const Rendition &r) {
        const auto it = std::find(o.video_codecs.begin(), o.video_codecs.end(), video_codec(r));
        return static_cast<int>(it - o.video_codecs.begin());
    };
    for (size_t i = 0; i < p.renditions.size(); ++i) {
        const Rendition &r = p.renditions[i];
        if (r.type != TrackType::Video || r.period > 0 || !decryptable(r, o.keys)) continue;
        if (rank(r) >= static_cast<int>(o.video_codecs.size())) continue;
        if (r.height > o.max_height || r.frame_rate > o.max_frame_rate + 0.01) continue;
        if (o.max_bandwidth > 0 && r.bandwidth > o.max_bandwidth) continue;
        if (!o.dolby_vision && !r.dolby_vision.empty() && any_plain) continue;
        // Profile 5 has no base layer a non-Dolby-Vision display can show (PlxNative refuses it too).
        if (!o.dolby_vision && dolby_vision_profile(r.dolby_vision) == 5) continue;
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const Rendition &b = p.renditions[static_cast<size_t>(best)];
        // Dolby Vision when wanted, then the larger picture, the preferred codec, the higher bitrate.
        const bool r_dv = o.dolby_vision && !r.dolby_vision.empty(), b_dv = o.dolby_vision && !b.dolby_vision.empty();
        if (r_dv != b_dv) {
            if (r_dv) best = static_cast<int>(i);
            continue;
        }
        if (r.height != b.height) {
            if (r.height > b.height) best = static_cast<int>(i);
            continue;
        }
        if (rank(r) != rank(b)) {
            if (rank(r) < rank(b)) best = static_cast<int>(i);
            continue;
        }
        if (r.bandwidth > b.bandwidth) best = static_cast<int>(i);
    }
    return best;
}

struct AdaptiveSource::Impl {
    AdaptiveSourceOptions opt;
    Presentation pres;
    MediaInfo info;
    struct AudioChoice {
        int rendition = -1;
        bool muxed = false;  // carried in the video rendition's segments
    };
    std::vector<AudioChoice> audio_choices;  // track index 1 + i
    int video = -1;                          // chosen video rendition (written under pres_mutex while live)

    // One FfmpegSource reading one rendition.
    struct Lane {
        int rendition = -1;
        // Its rendition in the first Period: a seek finds the Period to play from there.
        int head = -1;
        bool want_video = false, want_audio = false;
        std::unique_ptr<FfmpegSource> src;
        std::optional<Packet> pending;
        bool ended = false;
        net::SegmentReader *reader = nullptr;  // owned by src
        size_t first = 0;                      // segment the reader started at
        // A bitrate switch in progress: `reader` ends at a segment boundary, then this takes over.
        struct Switch {
            int rendition = -1;
            size_t first = 0;
            std::unique_ptr<net::SegmentReader> reader;
        };
        std::optional<Switch> next;
        // Packets ending before this are dropped: an audio lane reopened at its own segment boundary
        // may start well before the video's keyframe (segment lengths differ, and HLS EXTINF values
        // are only approximate).
        int64_t drop_before_us = kNoTime;
    };
    // Guards the renditions' segment lists, which the live refresher grows. Never held across a
    // blocking call. Order: pres_mutex before lanes_mutex.
    std::mutex pres_mutex;
    std::mutex lanes_mutex;  // guards the lane pointers against interrupt() from other threads
    std::vector<Lane> lanes;
    BackendCaps caps;
    int sel_audio = -1;        // rendition of a separate audio lane
    int64_t position_us = 0;   // where the next select() starts
    bool reopen = false;       // a seek was interrupted while reopening: reopen at position_us first
    int64_t hls_origin_us = kNoTime;
    // Live.
    bool live_ended = false;               // the presentation stopped being live (HLS ENDLIST, static MPD)
    int64_t zero_us = 0;                   // DASH: subtracted from presentation times (the window start at open)
    int64_t clock_offset_us = 0;           // server time minus this device's (DASH UTCTiming)
    std::map<long, int64_t> window_first;  // rendition → number of its oldest segment still listed
    // DASH: the MPD last fetched, re-read against the clock between fetches (refresh thread after open).
    std::string mpd_body, mpd_url;
    int64_t mpd_fetched_us = kNoTime;  // steady clock
    std::thread refresh_thread;
    std::mutex refresh_mutex;              // also guards live_subtitles and live_ladder
    std::vector<long> live_subtitles;      // subtitle renditions the refresher merges too (DASH)
    // DASH: the video renditions a bitrate switch may go to, merged too, so that the target's list
    // already holds the segment where the switch lands (HLS reloads the target's playlist instead).
    std::vector<long> live_ladder;
    std::condition_variable refresh_cv;
    std::vector<int> ladder;        // video renditions that can replace each other, bandwidth ascending
    std::vector<int> active_ladder; // the part of it the backend can follow (select)
    // The separate audio lane's renditions that differ only in bitrate, bandwidth ascending; fewer
    // than two: the audio stays as selected. DASH: Representations of one AdaptationSet with the same
    // codec, channels and rate, so the backend's audio format never changes. HLS: the same audio in
    // each audio group, one per group; it follows the video's variant.
    std::vector<int> audio_ladder;
    int follow_tried = -1;  // HLS: the audio rendition last tried for the video's variant
    ThroughputEstimator estimator;
    std::atomic<int64_t> limit_bps{0};
    // What the last decision of each lane saw (0 video, 1 audio): decide again only on news.
    int decided_samples[2] = {0, 0};
    int64_t decided_limit[2] = {0, 0};
    std::atomic<int> switch_count{0};
    std::atomic<int> audio_switch_count{0};
    std::atomic<bool> interrupted{false};
    std::mutex fetch_mutex;
    std::shared_ptr<async::CancellationSource> fetch_cancel = std::make_shared<async::CancellationSource>();

    // The subtitle loader's and the live refresher's own stop flags and cancellation: a seek
    // interrupts playback, not them.
    std::atomic<bool> subs_stop{false};
    std::shared_ptr<async::CancellationSource> subs_cancel = std::make_shared<async::CancellationSource>();
    std::atomic<bool> refresh_stop{false};
    std::shared_ptr<async::CancellationSource> refresh_cancel = std::make_shared<async::CancellationSource>();
    enum class Ctx : uint8_t { Playback, Subtitles, Refresh };

    // GET (optionally a byte range), with retries on transient failures.
    bool fetch(const std::string &url, const ByteRange *range, std::string *body, std::string *final_url,
               std::string *error, Ctx ctx = Ctx::Playback) {
        const std::atomic<bool> &stop = ctx == Ctx::Subtitles ? subs_stop : ctx == Ctx::Refresh ? refresh_stop : interrupted;
        if (!opt.http) {
            *error = "no HTTP transport";
            return false;
        }
        net::HttpRequest req;
        req.url = url;
        req.headers = opt.headers;
        req.max_response_bytes = 16u << 20;
        if (range != nullptr && !range->whole()) {
            std::string v = "bytes=" + std::to_string(range->offset) + "-";
            if (range->length >= 0) v += std::to_string(range->offset + range->length - 1);
            req.headers.emplace_back("Range", v);
        }
        async::CancellationToken token;
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            token = ctx == Ctx::Subtitles ? subs_cancel->token()
                    : ctx == Ctx::Refresh ? refresh_cancel->token()
                                          : fetch_cancel->token();
        }
        const net::RetryPolicy retry;
        for (int attempt = 1;; ++attempt) {
            if (stop) {
                *error = "interrupted";
                return false;
            }
            net::HttpResponse r = opt.http->perform(req, token);
            if (r.ok()) {
                *body = std::move(r.body);
                if (final_url != nullptr) *final_url = r.effective_url.empty() ? url : r.effective_url;
                if (range != nullptr && !range->whole() && r.status == 200) {
                    // The server ignored the range: cut it out of the whole body.
                    if (range->offset > static_cast<int64_t>(body->size())) {
                        *error = "range beyond the end of " + url;
                        return false;
                    }
                    *body = body->substr(static_cast<size_t>(range->offset),
                                         range->length >= 0 ? static_cast<size_t>(range->length) : std::string::npos);
                }
                return true;
            }
            if (stop || r.error == net::NetError::Cancelled) {
                *error = "interrupted";
                return false;
            }
            if (!retry.should_retry(r, attempt)) {
                *error = r.error != net::NetError::None ? std::string(net::error_name(r.error)) + " " + r.detail
                                                        : "HTTP " + std::to_string(r.status);
                *error += " for " + url;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(retry.backoff_ms(attempt)));
        }
    }

    // Loads an HLS media playlist or a DASH segment index if the rendition still needs one.
    bool ensure_segments(Rendition &r, std::string *error, Ctx ctx = Ctx::Playback) {
        {
            std::lock_guard<std::mutex> lock(pres_mutex);
            if (!r.needs_playlist() && !r.needs_index()) {
                if (r.segments.empty()) *error = "rendition " + r.id + " has no segments";
                return !r.segments.empty();
            }
        }
        if (r.needs_playlist()) {
            std::string body, final_url;
            if (!fetch(r.playlist_url, nullptr, &body, &final_url, error, ctx)) return false;
            Rendition fresh = r;
            bool live = false;
            if (!parse_hls_media(body, final_url, &fresh, &live, error)) return false;
            std::lock_guard<std::mutex> lock(pres_mutex);
            if (live) {
                if (!pres.live) pres.live = true;  // written once: other threads read it
                align_to_video(&r - pres.renditions.data(), fresh);
            }
            r.segments = std::move(fresh.segments);
            r.init = fresh.init;
            r.encrypted = fresh.encrypted;
            r.target_duration_us = fresh.target_duration_us;
            if (live && !r.segments.empty()) window_first[&r - pres.renditions.data()] = r.segments.front().number;
        } else if (r.needs_index()) {
            std::string body;
            if (!fetch(r.index_url, &r.index, &body, nullptr, error, ctx)) return false;
            std::vector<MediaSegment> segs;
            if (is_webm(r)) {
                // WebM: the index range holds Cues; the initialisation range says where the Segment's
                // data starts (Cue positions are relative to it) and the timecode scale.
                std::string init_body;
                if (!r.init) {
                    *error = "rendition " + r.id + ": WebM without an Initialization range";
                    return false;
                }
                if (!fetch(r.init->url, &r.init->range, &init_body, nullptr, error, ctx)) return false;
                if (!parse_webm_cues(reinterpret_cast<const uint8_t *>(init_body.data()), init_body.size(), r.init->range.offset,
                                     reinterpret_cast<const uint8_t *>(body.data()), body.size(), r.index.offset, r.index_url,
                                     &segs, error)) {
                    return false;
                }
            } else if (!parse_sidx(reinterpret_cast<const uint8_t *>(body.data()), body.size(), r.index.offset, r.index_url,
                                   &segs, error)) {
                return false;
            }
            const int64_t origin = r.time_origin_us != kNoTime ? r.time_origin_us : 0;
            for (MediaSegment &s : segs) s.start_us -= origin;
            std::lock_guard<std::mutex> lock(pres_mutex);
            r.segments = std::move(segs);
        }
        std::lock_guard<std::mutex> lock(pres_mutex);
        if (r.segments.empty()) {
            *error = "rendition " + r.id + " has no segments";
            return false;
        }
        return true;
    }

    std::shared_ptr<HlsKeyCache> hls_keys = std::make_shared<HlsKeyCache>();

    // A segment as a reader part; HLS AES-128 segments are decrypted as they arrive (the key is
    // fetched by the download thread, once per URI).
    net::SegmentPart part_of(const MediaSegment &s) const {
        net::SegmentPart p(s.url, s.range.offset, s.range.length);
        if (s.key && s.key->method == "AES-128") {
            p.filter = [cache = hls_keys, key = s.key, number = s.number](std::string *error) -> std::unique_ptr<net::PartFilter> {
                std::string bytes;
                if (!cache->get(key->uri, &bytes, error)) return nullptr;
                uint8_t iv[16];
                hls_iv(*key, number, iv);
                return std::make_unique<Aes128CbcFilter>(bytes, iv);
            };
        }
        return p;
    }

    std::vector<net::SegmentPart> parts_from(const Rendition &r, size_t first) const {
        std::vector<net::SegmentPart> parts;
        if (r.init) parts.push_back(part_of(*r.init));
        for (size_t i = first; i < r.segments.size(); ++i) parts.push_back(part_of(r.segments[i]));
        return parts;
    }

    // SAMPLE-AES and SAMPLE-AES-CTR: Common Encryption inside fMP4 ('cbcs' / 'cenc'), decrypted by the
    // demuxer with the key from the URI. SAMPLE-AES in MPEG-TS (Apple's own sample format) is not.
    bool sample_key(const Rendition &r, size_t first, std::string *hex, std::string *error) {
        std::shared_ptr<const HlsKey> key;
        {
            std::lock_guard<std::mutex> plock(pres_mutex);
            if (first < r.segments.size()) key = r.segments[first].key;
            else if (!r.segments.empty()) key = r.segments.back().key;
        }
        if (!key || key->method == "AES-128") return true;
        if (key->key_format != "identity") {
            *error = "HLS key format " + key->key_format + " is not supported (only identity keys)";
            return false;
        }
        if (!r.init) {
            *error = "HLS " + key->method + " in MPEG-TS is not supported (only in fMP4)";
            return false;
        }
        std::string bytes;
        if (!hls_keys->get(key->uri, &bytes, error)) return false;
        static const char *kDigits = "0123456789abcdef";
        hex->clear();
        for (const unsigned char c : bytes) {
            hex->push_back(kDigits[c >> 4]);
            hex->push_back(kDigits[c & 15]);
        }
        return true;
    }

    // A reader over `r` from segment `first`. The caller holds pres_mutex.
    std::unique_ptr<net::SegmentReader> make_reader(const Rendition &r, size_t first) {
        net::SegmentReaderOptions so = opt.segments;
        so.headers = opt.headers;
        so.on_part_done = [this](size_t, int64_t bytes, double seconds) { estimator.add(bytes, seconds); };
        int64_t seg_us = 0;
        for (const MediaSegment &s : r.segments) seg_us = std::max(seg_us, s.duration_us);
        so.max_parts_ahead = seg_us > 0 ? static_cast<size_t>(std::max<int64_t>(2, (opt.max_ahead_us + seg_us - 1) / seg_us))
                                        : 0;
        so.open_ended = pres.live && !live_ended;  // the refresher appends what the server publishes next
        return std::make_unique<net::SegmentReader>(opt.http, parts_from(r, first), so);
    }

    // Opens `lane` on its rendition from segment `first`, delivering what it wants; `fmt` gets what
    // the inner source will deliver.
    bool open_lane(Lane &lane, size_t first, StreamFormat *fmt, std::string *error,
                   std::unique_ptr<net::SegmentReader> prebuilt = nullptr) {
        const Rendition &r = pres.renditions[static_cast<size_t>(lane.rendition)];
        const bool hls = pres.format == Presentation::Format::Hls;
        std::string sample_key_hex;
        if (hls && !sample_key(r, first, &sample_key_hex, error)) {
            *error = "rendition " + r.id + ": " + *error;
            return false;
        }
        for (int pass = 0; pass < 2; ++pass) {
            std::unique_ptr<net::SegmentReader> reader;
            FfmpegSource *s = nullptr;
            std::string first_url;
            int64_t first_start = 0;
            {
                // The reader's parts and its registration in the lane under one lock, so a live
                // refresh appends exactly what came after this snapshot.
                std::lock_guard<std::mutex> plock(pres_mutex);
                const bool waiting = pres.live && !live_ended && first == r.segments.size() && !r.segments.empty();
                if (first >= r.segments.size() && !waiting) {
                    *error = "rendition " + r.id + ": no segment " + std::to_string(first);
                    return false;
                }
                reader = prebuilt ? std::move(prebuilt) : make_reader(r, first);
                // A live reader that starts after the list's end waits for the refresher's appends.
                const MediaSegment &at = waiting ? r.segments.back() : r.segments[first];
                first_url = at.url;
                first_start = waiting ? at.start_us + at.duration_us : at.start_us;
                FfmpegSourceOptions so;
                so.ac3_encode = opt.ac3_encode;
                so.decryption_keys = opt.keys;  // also for encryption the manifest does not declare
                so.decryption_default_key = sample_key_hex;
                auto src = std::make_unique<FfmpegSource>(std::move(so));
                src->set_time_origin(hls ? hls_origin_us : r.time_origin_us);
                s = src.get();
                std::lock_guard<std::mutex> lock(lanes_mutex);
                lane.src = std::move(src);
                lane.reader = reader.get();
                lane.first = first;
                lane.pending.reset();
                lane.ended = false;
                if (interrupted) s->interrupt();
            }
            if (!s->open_reader(std::move(reader), first_url, error)) {
                *error = "rendition " + r.id + ": " + *error;
                return false;
            }
            if (hls && hls_origin_us == kNoTime) {
                // All HLS renditions share the timeline of the video's first segment.
                hls_origin_us = s->time_origin() - first_start;
                if (first != 0 && pass == 0) continue;  // opened with the container's start: again
            }
            break;
        }
        FfmpegSource &s = *lane.src;
        const TrackInfo *v = lane.want_video ? s.info().best(TrackType::Video) : nullptr;
        const TrackInfo *a = lane.want_audio ? s.info().best(TrackType::Audio) : nullptr;
        if (lane.want_video && v == nullptr) {
            *error = "rendition " + r.id + " has no video stream";
            return false;
        }
        if (lane.want_audio && a == nullptr) {
            *error = "rendition " + r.id + " has no audio stream";
            return false;
        }
        if (!s.select(v != nullptr ? v->index : -1, a != nullptr ? a->index : -1, caps, fmt, error)) return false;
        // What the manifest says where the segments did not (no dvvC in the init segment, an E-AC-3
        // stream whose JOC FFmpeg has not met yet).
        if (fmt->video && fmt->video->dolby_vision_profile < 0 && fmt->video->codec == CodecId::Hevc) {
            const int p = dolby_vision_profile(r.dolby_vision);
            if (p >= 0 && p != 7) fmt->video->dolby_vision_profile = p;  // 7 is dual-layer
        }
        if (fmt->audio && fmt->audio->codec == CodecId::Eac3 && r.atmos) fmt->audio->atmos = true;
        return true;
    }

    // ---- live -------------------------------------------------------------------------------

    int64_t now_us() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
                   .count() +
               clock_offset_us;
    }

    // DASH: presentation times from the window start at open, so a live timeline reads like a VOD one.
    void normalize(Presentation &p) const {
        for (Rendition &r : p.renditions) {
            for (MediaSegment &s : r.segments) s.start_us -= zero_us;
            if (r.time_origin_us != kNoTime) r.time_origin_us += zero_us;
        }
    }

    // HLS: a live rendition's playlist times start at its own first segment; line them up with the
    // video's through a segment number both lists have. pres_mutex held.
    void align_to_video(long index, Rendition &fresh) {
        if (pres.format != Presentation::Format::Hls || video < 0 || index == video) return;
        const auto &ref = pres.renditions[static_cast<size_t>(video)].segments;
        for (const MediaSegment &s : fresh.segments) {
            auto it = std::find_if(ref.begin(), ref.end(), [&](const MediaSegment &v) { return v.number == s.number; });
            if (it == ref.end()) continue;
            const int64_t delta = it->start_us - s.start_us;
            for (MediaSegment &f : fresh.segments) f.start_us += delta;
            return;
        }
    }

    // Appends segments newer than `cur`'s last to it (HLS: timed on from its end) and to every
    // reader registered for that rendition; finishes them when the list has ended. pres_mutex held.
    void merge(long index, std::vector<MediaSegment> fresh, bool ended) {
        Rendition &cur = pres.renditions[static_cast<size_t>(index)];
        if (!fresh.empty()) window_first[index] = fresh.front().number;
        std::vector<net::SegmentPart> added;
        const int64_t last = cur.segments.empty() ? INT64_MIN : cur.segments.back().number;
        for (MediaSegment &s : fresh) {
            if (s.number <= last) continue;
            if (pres.format == Presentation::Format::Hls && !cur.segments.empty()) {
                s.start_us = cur.segments.back().start_us + cur.segments.back().duration_us;
            }
            added.push_back(part_of(s));
            cur.segments.push_back(std::move(s));
        }
        std::lock_guard<std::mutex> lock(lanes_mutex);
        for (Lane &l : lanes) {
            if (l.rendition == index && l.reader != nullptr) {
                if (!added.empty()) l.reader->append(added);
                if (ended) l.reader->finish();
            }
            if (l.next && l.next->rendition == index && l.next->reader) {
                if (!added.empty()) l.next->reader->append(added);
                if (ended) l.next->reader->finish();
            }
        }
    }

    // HLS: reloads one rendition's media playlist and merges it.
    bool refresh_rendition(long index, Ctx ctx, std::string *error) {
        Rendition fresh;
        std::string url;
        {
            std::lock_guard<std::mutex> lock(pres_mutex);
            fresh = pres.renditions[static_cast<size_t>(index)];
            url = fresh.playlist_url;
        }
        if (url.empty()) return true;
        std::string body, final_url;
        if (!fetch(url, nullptr, &body, &final_url, error, ctx)) return false;
        bool live = true;
        if (!parse_hls_media(body, final_url, &fresh, &live, error)) return false;
        std::lock_guard<std::mutex> lock(pres_mutex);
        if (pres.renditions[static_cast<size_t>(index)].segments.empty()) align_to_video(index, fresh);
        pres.renditions[static_cast<size_t>(index)].target_duration_us = fresh.target_duration_us;
        merge(index, std::move(fresh.segments), !live);
        if (!live && index == video) live_ended = true;
        return true;
    }

    static int64_t steady_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // DASH: merges the renditions in use from the MPD read at the current time. The MPD itself is
    // fetched again once minimumUpdatePeriod has passed (every time when it names none); in between,
    // the last one is read again, which lists the segments a SegmentTemplate@duration has made
    // available since (they appear on the clock, not with a new MPD). A failed fetch falls back to
    // the last MPD, so a slow server does not starve playback.
    bool refresh_dash(const std::vector<long> &active, std::string *error) {
        const int64_t update = pres.update_period_us;
        const bool due = mpd_body.empty() || update == kNoTime || update <= 0 || mpd_fetched_us == kNoTime ||
                         steady_us() - mpd_fetched_us >= update;
        bool fetch_failed = false;
        if (due) {
            std::string body, final_url;
            if (fetch(pres.url, nullptr, &body, &final_url, error, Ctx::Refresh)) {
                mpd_body = std::move(body);
                mpd_url = std::move(final_url);
                mpd_fetched_us = steady_us();
            } else if (mpd_body.empty() || refresh_stop) {
                return false;
            } else {
                fetch_failed = true;
            }
        }
        Presentation fresh;
        if (!parse_mpd(mpd_body, mpd_url, &fresh, error, now_us())) return false;
        normalize(fresh);
        std::lock_guard<std::mutex> lock(pres_mutex);
        for (long i : active) {
            const Rendition &cur = pres.renditions[static_cast<size_t>(i)];
            auto it = std::find_if(fresh.renditions.begin(), fresh.renditions.end(),
                                   [&](const Rendition &r) { return r.id == cur.id && r.group == cur.group; });
            if (it == fresh.renditions.end()) continue;
            merge(i, std::move(it->segments), !fresh.live);
        }
        if (!fresh.live) live_ended = true;
        return !fetch_failed;  // *error says why the fetch failed; the last MPD was used
    }

    void refresh_loop() {
        while (!refresh_stop) {
            int64_t period;
            {
                std::lock_guard<std::mutex> lock(pres_mutex);
                int64_t seg = 0;
                if (video >= 0) {
                    for (const MediaSegment &s : pres.renditions[static_cast<size_t>(video)].segments) seg = std::max(seg, s.duration_us);
                    if (pres.format == Presentation::Format::Hls && pres.renditions[static_cast<size_t>(video)].target_duration_us > 0) {
                        seg = pres.renditions[static_cast<size_t>(video)].target_duration_us;
                    }
                }
                // DASH: every segment duration (new segments of a @duration template appear on the
                // clock), the MPD itself fetched again every minimumUpdatePeriod (refresh_dash).
                const bool dash_update = pres.format == Presentation::Format::Dash && pres.update_period_us != kNoTime &&
                                         pres.update_period_us > 0;
                if (dash_update) {
                    period = seg > 0 ? std::min(seg, pres.update_period_us) : pres.update_period_us;
                } else {
                    period = seg > 0 ? seg : 2000000;
                }
                period = std::clamp<int64_t>(period, 500000, 30000000);
            }
            {
                std::unique_lock<std::mutex> lock(refresh_mutex);
                refresh_cv.wait_for(lock, std::chrono::microseconds(period), [&] { return refresh_stop.load(); });
            }
            if (refresh_stop) return;
            std::vector<long> active;
            {
                std::lock_guard<std::mutex> lock(lanes_mutex);
                for (const Lane &l : lanes) {
                    active.push_back(l.rendition);
                    if (l.next) active.push_back(l.next->rendition);
                }
            }
            if (pres.format == Presentation::Format::Dash) {
                std::lock_guard<std::mutex> lock(refresh_mutex);
                active.insert(active.end(), live_subtitles.begin(), live_subtitles.end());
                active.insert(active.end(), live_ladder.begin(), live_ladder.end());
                std::sort(active.begin(), active.end());
                active.erase(std::unique(active.begin(), active.end()), active.end());
            }
            std::string error;
            bool ok = true;
            if (pres.format == Presentation::Format::Dash) {
                ok = refresh_dash(active, &error);
            } else {
                for (long i : active) ok = refresh_rendition(i, Ctx::Refresh, &error) && ok;
            }
            if (!ok && !refresh_stop) WNK_LOGW(kCat, "live refresh: %s", error.c_str());
            std::lock_guard<std::mutex> lock(pres_mutex);
            if (live_ended) {
                WNK_LOGI(kCat, "the live presentation has ended");
                return;
            }
        }
    }

    void start_refresh() {
        if (!pres.live || live_ended || refresh_thread.joinable()) return;
        refresh_stop = false;
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            refresh_cancel = std::make_shared<async::CancellationSource>();
        }
        refresh_thread = std::thread([this] { refresh_loop(); });
    }

    void stop_refresh() {
        {
            std::lock_guard<std::mutex> lock(refresh_mutex);
            refresh_stop = true;
            refresh_cv.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            refresh_cancel->cancel();
        }
        if (refresh_thread.joinable()) refresh_thread.join();
    }

    // DASH UTCTiming: the server's clock, so "now" matches what it publishes (http-xsdate, http-iso,
    // direct). The request's latency is not corrected for.
    void sync_clock() {
        const std::string &scheme = pres.utc_timing_scheme;
        std::string text;
        if (scheme.find(":http-xsdate:") != std::string::npos || scheme.find(":http-iso:") != std::string::npos) {
            std::string error;
            if (!fetch(resolve_url(pres.url, pres.utc_timing_value), nullptr, &text, nullptr, &error)) {
                WNK_LOGW(kCat, "UTCTiming: %s; using this device's clock", error.c_str());
                return;
            }
        } else if (scheme.find(":direct:") != std::string::npos) {
            text = pres.utc_timing_value;
        } else {
            return;
        }
        int64_t server = 0;
        if (!parse_iso_datetime(text, &server)) return;
        const int64_t local =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        clock_offset_us = server - local;
        WNK_LOGI(kCat, "server clock %+lld ms from this device's", static_cast<long long>(clock_offset_us / 1000));
    }

    // Video renditions that can stand in for `base` mid-stream, bandwidth ascending.
    void build_ladder(int base) {
        ladder.clear();
        const Rendition &b = pres.renditions[static_cast<size_t>(base)];
        for (size_t i = 0; i < pres.renditions.size(); ++i) {
            const Rendition &r = pres.renditions[i];
            if (r.type != TrackType::Video || r.period > 0 || !decryptable(r, opt.keys)) continue;
            if (video_codec(r) != video_codec(b) || r.dolby_vision != b.dolby_vision) continue;
            if (r.height > opt.max_height || r.frame_rate > opt.max_frame_rate + 0.01) continue;
            if (opt.max_bandwidth > 0 && r.bandwidth > opt.max_bandwidth) continue;
            if (pres.format == Presentation::Format::Dash ? r.group != b.group : r.muxed_audio != b.muxed_audio) continue;
            // HLS lists one video playlist once per audio group: keep it once.
            const bool dup = std::any_of(ladder.begin(), ladder.end(), [&](int k) {
                const Rendition &o = pres.renditions[static_cast<size_t>(k)];
                return pres.format == Presentation::Format::Hls ? o.playlist_url == r.playlist_url : o.id == r.id;
            });
            if (!dup) ladder.push_back(static_cast<int>(i));
        }
        std::stable_sort(ladder.begin(), ladder.end(), [&](int x, int y) {
            return pres.renditions[static_cast<size_t>(x)].bandwidth < pres.renditions[static_cast<size_t>(y)].bandwidth;
        });
        if (std::find(ladder.begin(), ladder.end(), base) == ladder.end()) ladder = {base};
    }

    // Bandwidth of what a lane plays, or is about to play once its switch lands.
    int64_t lane_bandwidth(const Lane &l) const {
        const int r = l.next ? l.next->rendition : l.rendition;
        return r >= 0 ? pres.renditions[static_cast<size_t>(r)].bandwidth : 0;
    }

    // Audio renditions that can replace `base` mid-stream (see audio_ladder), bandwidth ascending.
    void build_audio_ladder(int base) {
        audio_ladder.clear();
        const Rendition &b = pres.renditions[static_cast<size_t>(base)];
        const std::string codec = lower(codec_entry(b.codecs, TrackType::Audio));
        if (pres.format == Presentation::Format::Hls) {
            if (b.group.empty()) return;  // an audio-only variant: no groups to move between
            // Groups may name the same audio differently (ffmpeg: "audio_2", "audio_3"): match on
            // language, codec and channels, and on NAME only to choose among several in one group.
            std::map<std::string, std::vector<int>> by_group;
            for (size_t i = 0; i < pres.renditions.size(); ++i) {
                const Rendition &r = pres.renditions[i];
                if (r.type != TrackType::Audio || r.group.empty() || r.language != b.language || r.channels != b.channels ||
                    lower(codec_entry(r.codecs, TrackType::Audio)) != codec) {
                    continue;
                }
                by_group[r.group].push_back(static_cast<int>(i));
            }
            for (const auto &[group, ks] : by_group) {
                int pick = ks.size() == 1 ? ks[0] : -1;
                for (int k : ks) {
                    if (k == base || (pick < 0 && pres.renditions[static_cast<size_t>(k)].label == b.label)) pick = k;
                }
                if (pick >= 0) audio_ladder.push_back(pick);  // several and none by that name: not guessed
            }
        }
        for (size_t i = 0; i < pres.renditions.size() && pres.format == Presentation::Format::Dash; ++i) {
            const Rendition &r = pres.renditions[i];
            if (r.type != TrackType::Audio || r.period != b.period || r.group != b.group || !decryptable(r, opt.keys)) continue;
            if (lower(codec_entry(r.codecs, TrackType::Audio)) != codec || r.channels != b.channels ||
                r.sample_rate != b.sample_rate) {
                continue;
            }
            if (opt.max_bandwidth > 0 && r.bandwidth > opt.max_bandwidth && static_cast<int>(i) != base) continue;
            const bool dup = std::any_of(audio_ladder.begin(), audio_ladder.end(),
                                         [&](int k) { return pres.renditions[static_cast<size_t>(k)].id == r.id; });
            if (!dup) audio_ladder.push_back(static_cast<int>(i));
        }
        std::stable_sort(audio_ladder.begin(), audio_ladder.end(), [&](int x, int y) {
            return pres.renditions[static_cast<size_t>(x)].bandwidth < pres.renditions[static_cast<size_t>(y)].bandwidth;
        });
    }

    // Starts bitrate switches when the estimate calls for another rung: the video lane over the
    // whole estimate, then a separate audio lane over what the video leaves of it (and of the limit).
    void maybe_switch() {
        if (!opt.adaptive_bitrate || lanes.empty()) return;
        const int64_t estimate = estimator.estimate_bps();
        const int64_t limit = limit_bps.load();
        if (lanes[0].want_video) maybe_switch_lane(0, active_ladder, estimate, limit, 0);
        Lane &a = lanes.back();
        if (pres.format == Presentation::Format::Hls) {
            // The variant's BANDWIDTH counts its own group's audio: the audio follows the video.
            if (audio_ladder.size() > 1 && lanes.size() > 1 && lanes[0].want_video && a.want_audio && !a.next &&
                a.reader != nullptr && !a.ended) {
                const int want = variant_audio(lanes[0].next ? lanes[0].next->rendition : lanes[0].rendition, a.rendition);
                if (want != a.rendition && want != follow_tried) {
                    follow_tried = want;  // once per change: a failed or too-late switch is not retried
                    start_switch(lanes.size() - 1, want, "following the video");
                }
            }
            return;
        }
        if (audio_ladder.size() > 1 && !a.want_video && a.want_audio) {
            const int64_t video_bw = lanes[0].want_video ? lane_bandwidth(lanes[0]) : 0;
            const int64_t left = estimate > 0 ? std::max<int64_t>(1, estimate - video_bw) : 0;
            const int64_t left_limit = limit > 0 ? std::max<int64_t>(1, limit - video_bw) : 0;
            maybe_switch_lane(lanes.size() - 1, audio_ladder, left, left_limit, 1);
        }
    }

    void maybe_switch_lane(size_t index, const std::vector<int> &rungs, int64_t estimate, int64_t limit, int slot) {
        if (rungs.size() < 2) return;
        Lane &v = lanes[index];
        if (v.next || v.reader == nullptr || v.ended) return;
        const int samples = estimator.samples();
        const int64_t raw_limit = limit_bps.load();
        if (samples == decided_samples[slot] && raw_limit == decided_limit[slot]) return;  // nothing new to decide on
        decided_samples[slot] = samples;
        decided_limit[slot] = raw_limit;
        const auto at = std::find(rungs.begin(), rungs.end(), v.rendition);
        if (at == rungs.end()) return;
        std::vector<int64_t> bws;
        for (int k : rungs) bws.push_back(pres.renditions[static_cast<size_t>(k)].bandwidth);
        const int current = static_cast<int>(at - rungs.begin());
        const int target = choose_rung(bws, current, estimate, limit);
        if (target == current || target < 0) return;
        start_switch(index, rungs[static_cast<size_t>(target)], ("throughput " + mbps(estimate)).c_str());
    }

    // HLS: the audio rendition that plays beside `variant`: `audio` if its group serves the variant
    // (one video playlist may be listed once per group), else its rung in a group that does.
    int variant_audio(int variant, int audio) const {
        const Rendition &v = pres.renditions[static_cast<size_t>(variant)];
        auto serves = [&](const std::string &group) {
            return std::any_of(pres.renditions.begin(), pres.renditions.end(), [&](const Rendition &o) {
                return o.type == TrackType::Video && o.audio_group == group && o.playlist_url == v.playlist_url;
            });
        };
        if (serves(pres.renditions[static_cast<size_t>(audio)].group)) return audio;
        int found = audio;
        for (int k : audio_ladder) {
            const std::string &g = pres.renditions[static_cast<size_t>(k)].group;
            if (g == v.audio_group) return k;
            if (found == audio && serves(g)) found = k;
        }
        return found;
    }

    // Starts switching lane `index` to rendition `to_index` at its next segment boundary.
    void start_switch(size_t index, int to_index, const char *reason) {
        Lane &v = lanes[index];
        const char *what = v.want_video ? "video" : "audio";
        Rendition &to = pres.renditions[static_cast<size_t>(to_index)];
        std::string why;
        if (!ensure_segments(to, &why)) {
            WNK_LOGW(kCat, "cannot switch %s to %s: %s", what, to.id.c_str(), why.c_str());
            return;
        }
        if (pres.live && pres.format == Presentation::Format::Hls && !refresh_rendition(to_index, Ctx::Playback, &why)) {
            WNK_LOGW(kCat, "cannot switch %s to %s: %s", what, to.id.c_str(), why.c_str());
            return;
        }
        std::lock_guard<std::mutex> plock(pres_mutex);
        const Rendition &from = pres.renditions[static_cast<size_t>(v.rendition)];
        // Live HLS: the boundary must exist in the target's list too (by number) before cutting.
        const size_t delivered = v.reader->stop_after_current();
        const size_t init = from.init ? 1 : 0;
        const size_t boundary = v.first + (delivered > init ? delivered - init : 0);  // first segment not read
        if (boundary >= from.segments.size()) {
            if (!pres.live || live_ended) return;  // the old one plays to the end
        }
        size_t first;
        if (boundary < from.segments.size() && from.segments[boundary].number >= 0 && pres.live) {
            const int64_t want = from.segments[boundary].number;
            auto it = std::find_if(to.segments.begin(), to.segments.end(), [&](const MediaSegment &s) { return s.number == want; });
            if (it == to.segments.end()) {
                // Not expected: the refresher keeps every rendition of the ladder listed.
                WNK_LOGW(kCat, "switch to %s: segment %lld not listed (last %lld), placed by time", to.id.c_str(),
                         static_cast<long long>(want), static_cast<long long>(to.segments.empty() ? -1 : to.segments.back().number));
            }
            first = it != to.segments.end() ? static_cast<size_t>(it - to.segments.begin())
                                            : segment_at(to.segments, from.segments[boundary].start_us + 1000);
        } else if (boundary >= from.segments.size()) {
            // Live, caught up with the list: the target continues after the last segment read.
            const int64_t want = from.segments.empty() ? -1 : from.segments.back().number + 1;
            auto it = std::find_if(to.segments.begin(), to.segments.end(), [&](const MediaSegment &s) { return s.number == want; });
            first = it != to.segments.end() ? static_cast<size_t>(it - to.segments.begin()) : to.segments.size();
        } else {
            first = from.segments.size() == to.segments.size() ? boundary
                                                               : segment_at(to.segments, from.segments[boundary].start_us + 1000);
        }
        Lane::Switch sw;
        sw.rendition = to_index;
        sw.first = first;
        sw.reader = make_reader(to, first);  // prefetches while the old rendition plays out
        std::lock_guard<std::mutex> lock(lanes_mutex);
        v.next = std::move(sw);
        WNK_LOGI(kCat, "%s: switching %s %s -> %s at segment %zu", reason, what, mbps(from.bandwidth).c_str(),
                 mbps(to.bandwidth).c_str(), boundary);
    }

    void close_lanes() {
        std::vector<Lane> gone;
        {
            std::lock_guard<std::mutex> lock(lanes_mutex);
            gone.swap(lanes);
        }
    }

    // Next non-subtitle packet of a lane (subtitles in segments are not offered yet).
    MediaSource::Read pull(Lane &lane, std::string *error) {
        while (!lane.pending && !lane.ended) {
            Packet p;
            const MediaSource::Read r = lane.src->read(&p, error);
            if (r == MediaSource::Read::EndOfStream && lane.next) {
                // The old rendition ended at the switch boundary: the new one continues from there.
                Lane::Switch sw;
                const int from = lane.rendition;
                {
                    // Keep the new reader reachable by the refresher while it changes hands.
                    std::lock_guard<std::mutex> plock(pres_mutex);
                    std::lock_guard<std::mutex> lock(lanes_mutex);
                    sw = std::move(*lane.next);
                    lane.next.reset();
                    lane.rendition = sw.rendition;
                    lane.reader = sw.reader.get();
                }
                StreamFormat fmt;
                if (!open_lane(lane, sw.first, &fmt, error, std::move(sw.reader))) {
                    if (interrupted) return MediaSource::Read::Interrupted;
                    return MediaSource::Read::Error;
                }
                ++switch_count;
                if (lane.want_video) {
                    std::lock_guard<std::mutex> plock(pres_mutex);  // the refresher reads it
                    video = sw.rendition;                           // a later select() keeps this quality
                } else {
                    ++audio_switch_count;
                    sel_audio = sw.rendition;
                }
                if (pres.renditions[static_cast<size_t>(sw.rendition)].period == 0) lane.head = sw.rendition;
                const Rendition &a = pres.renditions[static_cast<size_t>(from)];
                const Rendition &b = pres.renditions[static_cast<size_t>(sw.rendition)];
                if (lane.want_video) {
                    WNK_LOGI(kCat, "video %s (%dx%d %s) -> %s (%dx%d %s) at segment %zu", a.id.c_str(), a.width, a.height,
                             mbps(a.bandwidth).c_str(), b.id.c_str(), b.width, b.height, mbps(b.bandwidth).c_str(), sw.first);
                } else {
                    WNK_LOGI(kCat, "audio %s (%s) -> %s (%s) at segment %zu", a.id.c_str(), mbps(a.bandwidth).c_str(),
                             b.id.c_str(), mbps(b.bandwidth).c_str(), sw.first);
                }
                continue;
            }
            if (r == MediaSource::Read::EndOfStream && !lane.next) {
                // The Period ended: its continuation in the next Period (own initialisation segment
                // and timeline offset) plays from its first segment.
                const int next = pres.renditions[static_cast<size_t>(lane.rendition)].next_period;
                if (next >= 0) {
                    Rendition &to = pres.renditions[static_cast<size_t>(next)];
                    if (!ensure_segments(to, error)) {
                        if (interrupted) return MediaSource::Read::Interrupted;
                        return MediaSource::Read::Error;
                    }
                    const int from = lane.rendition;
                    {
                        std::lock_guard<std::mutex> lock(lanes_mutex);
                        lane.rendition = next;
                    }
                    StreamFormat fmt;
                    if (!open_lane(lane, 0, &fmt, error)) {
                        if (interrupted) return MediaSource::Read::Interrupted;
                        return MediaSource::Read::Error;
                    }
                    lane.drop_before_us = to.period_start_us;  // its first segment may begin earlier
                    WNK_LOGI(kCat, "period %d -> %d: %s %s", pres.renditions[static_cast<size_t>(from)].period, to.period,
                             track_type_name(to.type), to.id.c_str());
                    continue;
                }
            }
            if (r == MediaSource::Read::EndOfStream) {
                lane.ended = true;
            } else if (r != MediaSource::Read::Packet) {
                return r;
            } else if (p.type != TrackType::Subtitle) {
                // A Period's segments may run past its end; its continuation has what comes next.
                const Rendition &cur = pres.renditions[static_cast<size_t>(lane.rendition)];
                if (cur.next_period >= 0 && cur.period_end_us != kNoTime && p.pts_us != kNoTime && p.pts_us >= cur.period_end_us) {
                    continue;
                }
                if (lane.drop_before_us != kNoTime && p.pts_us != kNoTime &&
                    p.pts_us + std::max<int64_t>(p.duration_us, 1) <= lane.drop_before_us) {
                    continue;
                }
                lane.drop_before_us = kNoTime;
                lane.pending = std::move(p);
            }
        }
        return MediaSource::Read::Packet;
    }

    // ClearKey: the key IDs the encrypted renditions name that the app did not give a key for are
    // asked from the licence server (the app's, else the manifest's). opt.keys then holds every key
    // known; renditions still without theirs stay unplayable, and key_error says why.
    std::string key_error;
    void resolve_keys() {
        key_error.clear();
        std::vector<std::string> missing;
        std::string url = opt.license_url;
        size_t encrypted = 0;
        for (const Rendition &r : pres.renditions) {
            if (!r.encrypted) continue;
            ++encrypted;
            for (const std::string &k : r.key_ids) {
                if (opt.keys.count(k) == 0 && std::find(missing.begin(), missing.end(), k) == missing.end()) {
                    missing.push_back(k);
                }
            }
            if (url.empty()) url = r.license_url;
        }
        if (encrypted == 0) return;
        WNK_LOGI(kCat, "%zu encrypted rendition(s) (%s), %zu key(s) given, %zu key ID(s) without a key%s",
                 encrypted, pres.renditions.empty() ? "" : first_scheme().c_str(), opt.keys.size(), missing.size(),
                 missing.empty() || url.empty() ? "" : ": asking the licence server");
        if (missing.empty()) return;
        if (url.empty() || !opt.fetch_licenses || !opt.http) {
            key_error = "no key for key ID " + missing.front() + (url.empty() ? " and no licence server" : "");
            return;
        }
        async::CancellationToken token;
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            token = fetch_cancel->token();
        }
        std::string err;
        if (!fetch_clearkey_license(*opt.http, url, opt.license_headers, missing, token, &opt.keys, &err)) {
            WNK_LOGW(kCat, "ClearKey licence: %s", err.c_str());
            key_error = err;
        }
    }
    std::string first_scheme() const {
        for (const Rendition &r : pres.renditions) {
            if (r.encrypted && !r.protection_scheme.empty()) return r.protection_scheme;
        }
        return "scheme not stated";
    }

    // (Re)opens the selected renditions at `t`.
    // The rendition of `head`'s chain (Rendition::next_period) whose Period holds `t`.
    int locate(int head, int64_t t, std::string *error) {
        int r = head;
        for (;;) {
            Rendition &cur = pres.renditions[static_cast<size_t>(r)];
            if (cur.next_period < 0) return r;
            if (!ensure_segments(cur, error)) return -1;
            int64_t end;
            {
                std::lock_guard<std::mutex> plock(pres_mutex);
                end = cur.segments.empty() ? 0 : cur.segments.back().start_us + cur.segments.back().duration_us;
            }
            if (t < end) return r;
            r = cur.next_period;
            if (!ensure_segments(pres.renditions[static_cast<size_t>(r)], error)) return -1;
        }
    }

    bool open_at(int64_t t, StreamFormat *out, std::string *error) {
        if (lanes.empty()) return false;
        for (Lane &lane : lanes) {
            if (lane.head < 0 || pres.renditions[static_cast<size_t>(lane.head)].next_period < 0) continue;
            const int r = locate(lane.head, t, error);
            if (r < 0) return false;
            std::lock_guard<std::mutex> lock(lanes_mutex);
            lane.rendition = r;
        }
        // Where each lane starts: the first leads (video if any); the others start at or before its
        // segment.
        std::vector<size_t> firsts(lanes.size());
        {
            std::lock_guard<std::mutex> plock(pres_mutex);
            int64_t main_start = t;
            for (size_t i = 0; i < lanes.size(); ++i) {
                const auto &segs = pres.renditions[static_cast<size_t>(lanes[i].rendition)].segments;
                firsts[i] = i == 0 ? segment_at(segs, t) : segment_at(segs, main_start);
                if (i == 0 && firsts[i] < segs.size()) main_start = segs[firsts[i]].start_us;
            }
            std::lock_guard<std::mutex> lock(lanes_mutex);
            for (Lane &lane : lanes) lane.next.reset();
        }
        // The other lanes open alongside the first: each open is a few round trips (init segment,
        // first media segment). Not on the first HLS open, whose timeline origin the first lane sets.
        const bool parallel = lanes.size() > 1 && !(pres.format == Presentation::Format::Hls && hls_origin_us == kNoTime);
        struct Opened {
            bool ok = false;
            StreamFormat fmt;
            std::string error;
        };
        std::vector<Opened> others(lanes.size());
        std::vector<std::thread> openers;
        if (parallel) {
            for (size_t i = 1; i < lanes.size(); ++i) {
                openers.emplace_back([this, i, &firsts, &others] {
                    others[i].ok = open_lane(lanes[i], firsts[i], &others[i].fmt, &others[i].error);
                });
            }
        }
        auto join = [&] {
            for (std::thread &th : openers) th.join();
            openers.clear();
        };
        auto merge_fmt = [&](const StreamFormat &fmt) {
            if (out == nullptr) return;
            if (fmt.video) out->video = fmt.video;
            if (fmt.audio) out->audio = fmt.audio;
        };
        int64_t main_first_pts = kNoTime;
        {
            Lane &lane = lanes[0];
            StreamFormat fmt;
            if (!open_lane(lane, firsts[0], &fmt, error)) {
                join();
                return false;
            }
            merge_fmt(fmt);
            if (lane.want_video) {
                // Where the video really starts (its keyframe), for trimming the other lanes.
                const MediaSource::Read r = pull(lane, error);
                if (r != MediaSource::Read::Packet) {
                    if (r == MediaSource::Read::Interrupted) *error = "interrupted";
                    join();
                    return false;
                }
                if (lane.pending) main_first_pts = lane.pending->pts_us;
            }
        }
        join();
        for (size_t i = 1; i < lanes.size(); ++i) {
            if (!parallel) {
                others[i].ok = open_lane(lanes[i], firsts[i], &others[i].fmt, &others[i].error);
            }
            if (!others[i].ok) {
                *error = others[i].error;
                return false;
            }
            merge_fmt(others[i].fmt);
            lanes[i].drop_before_us = main_first_pts;
        }
        position_us = t;
        return true;
    }

    // ---- subtitles from the manifest -------------------------------------------------------
    enum class SubKind : uint8_t { None, VttFiles, TtmlFiles, Mp4Vtt, Mp4Ttml };
    struct SubtitleChoice {
        int rendition = -1;
        int track = -1;  // TrackInfo index
        SubKind kind = SubKind::None;
    };
    std::vector<SubtitleChoice> subtitle_choices;
    std::thread subs_thread;
    std::mutex subs_mutex;  // guards cue_queue
    std::deque<std::shared_ptr<const SubtitleCue>> cue_queue;
    std::condition_variable subs_cv;  // a cue arrived or the loader finished
    bool subs_running = false;         // guarded by subs_mutex

    SubKind subtitle_kind_of(const Rendition &r) const {
        const std::string mime = lower(r.mime_type), codecs = lower(r.codecs);
        if (pres.format == Presentation::Format::Hls) return SubKind::VttFiles;  // WebVTT is HLS's format
        if (mime == "text/vtt") return SubKind::VttFiles;
        if (mime == "application/ttml+xml") return SubKind::TtmlFiles;
        if (mime == "application/mp4" && codecs.rfind("wvtt", 0) == 0) return SubKind::Mp4Vtt;
        if (mime == "application/mp4" && codecs.rfind("stpp", 0) == 0) return SubKind::Mp4Ttml;
        return SubKind::None;
    }

    void push_cue(SubtitleCue c) {
        std::lock_guard<std::mutex> lock(subs_mutex);
        cue_queue.push_back(std::make_shared<SubtitleCue>(std::move(c)));
        subs_cv.notify_all();
    }

    // One subtitle track being loaded: where it has got to.
    struct SubState {
        SubtitleChoice choice;
        size_t next = 0;           // first segment not fetched yet
        bool failed = false;
        uint32_t timescale = 0;    // MP4: from the init segment, once
        size_t cues = 0;
    };

    // Fetches and parses the segments listed since the last call. False when the track gave up.
    bool load_new(SubState &st) {
        Rendition &r = pres.renditions[static_cast<size_t>(st.choice.rendition)];
        std::string error, body;
        auto fail = [&] {
            if (!subs_stop) WNK_LOGW(kCat, "subtitles %s: %s", r.id.c_str(), error.c_str());
            st.failed = true;
            return false;
        };
        const bool mp4 = st.choice.kind == SubKind::Mp4Vtt || st.choice.kind == SubKind::Mp4Ttml;
        if (mp4 && st.timescale == 0) {
            std::optional<MediaSegment> init;
            {
                std::lock_guard<std::mutex> lock(pres_mutex);
                init = r.init;
            }
            // Without an init segment (one fragmented file as the BaseURL), the moov is in the segment.
            if (init && (!fetch(init->url, &init->range, &body, nullptr, &error, Ctx::Subtitles) ||
                         !parse_wvtt_init(reinterpret_cast<const uint8_t *>(body.data()), body.size(), &st.timescale, &error))) {
                return fail();
            }
        }
        std::vector<MediaSegment> todo;
        int64_t origin = 0;
        {
            std::lock_guard<std::mutex> lock(pres_mutex);
            for (size_t i = st.next; i < r.segments.size(); ++i) todo.push_back(r.segments[i]);
            origin = mp4 ? (r.time_origin_us != kNoTime ? r.time_origin_us : 0) : (hls_origin_us != kNoTime ? hls_origin_us : 0);
        }
        std::optional<SubtitleCue> held;  // MP4: a cue split across fragments, joined again
        for (const MediaSegment &seg : todo) {
            if (subs_stop) return false;
            if (!fetch(seg.url, &seg.range, &body, nullptr, &error, Ctx::Subtitles)) return fail();
            ++st.next;
            if (mp4) {
                const auto *bytes = reinterpret_cast<const uint8_t *>(body.data());
                if (st.timescale == 0 && !parse_wvtt_init(bytes, body.size(), &st.timescale, &error)) {
                    error = "no initialisation segment";
                    return fail();
                }
                std::vector<SubtitleCue> got;
                if (!(st.choice.kind == SubKind::Mp4Vtt ? parse_wvtt_fragment(bytes, body.size(), st.timescale, &got, &error)
                                                         : parse_stpp_fragment(bytes, body.size(), st.timescale, &got, &error))) {
                    return fail();
                }
                for (SubtitleCue &cue : got) {
                    cue.track = st.choice.track;
                    cue.start_us -= origin;
                    if (cue.end_us != kNoTime) cue.end_us -= origin;
                    if (held && held->text == cue.text && std::llabs(held->end_us - cue.start_us) <= 1000) {
                        held->end_us = cue.end_us;
                        continue;
                    }
                    if (held) push_cue(std::move(*held)), ++st.cues;
                    held = std::move(cue);
                }
                continue;
            }
            if (st.choice.kind == SubKind::TtmlFiles) {
                TtmlFile f;
                if (!parse_ttml(body, &f, &error)) {
                    WNK_LOGW(kCat, "subtitles %s: %s", r.id.c_str(), error.c_str());
                    continue;
                }
                for (SubtitleCue &cue : f.cues) {  // presentation time, as DASH text files are
                    cue.track = st.choice.track;
                    push_cue(std::move(cue));
                    ++st.cues;
                }
                continue;
            }
            WebVttFile f;
            if (!parse_webvtt(body, &f, &error)) continue;
            // HLS: X-TIMESTAMP-MAP ties cue time LOCAL to media time MPEGTS (90 kHz), the clock the
            // video's timestamps also run on; without it cue times are taken as presentation time.
            int64_t offset = 0;
            if (pres.format == Presentation::Format::Hls && f.has_timestamp_map) {
                offset = f.map_mpegts * 1000000 / 90000 - f.map_local_us - origin;
            }
            for (SubtitleCue &cue : f.cues) {
                cue.track = st.choice.track;
                cue.start_us += offset;
                if (cue.end_us != kNoTime) cue.end_us += offset;
                push_cue(std::move(cue));
                ++st.cues;
            }
        }
        // Live: a held cue goes out now rather than waiting for the next refresh; its continuation,
        // if any, follows as a cue of its own.
        if (held) push_cue(std::move(*held)), ++st.cues;
        return true;
    }

    // The subtitle loader: every readable track once (VOD), or (live) what each list gains as the
    // presentation runs, from shortly before the join point.
    void load_subtitles(std::vector<SubtitleChoice> todo, int64_t join_us) {
        std::vector<SubState> states;
        for (const SubtitleChoice &c : todo) {
            Rendition &r = pres.renditions[static_cast<size_t>(c.rendition)];
            std::string error;
            if (!ensure_segments(r, &error, Ctx::Subtitles)) {
                if (!subs_stop) WNK_LOGW(kCat, "subtitles %s: %s", r.id.c_str(), error.c_str());
                continue;
            }
            SubState st;
            st.choice = c;
            if (pres.live) {
                std::lock_guard<std::mutex> lock(pres_mutex);
                st.next = segment_at(r.segments, std::max<int64_t>(0, join_us - 10000000));
            }
            states.push_back(st);
        }
        if (pres.live) {
            std::lock_guard<std::mutex> lock(refresh_mutex);
            for (const SubState &st : states) live_subtitles.push_back(st.choice.rendition);  // DASH: merged by the refresher
        }
        for (;;) {
            for (SubState &st : states) {
                if (subs_stop) return;
                if (!st.failed) load_new(st);
            }
            bool ended;
            {
                std::lock_guard<std::mutex> lock(pres_mutex);
                ended = !pres.live || live_ended;
            }
            if (ended || subs_stop) break;
            // Live: wait a segment's time, then (HLS) reload the subtitle playlists; a DASH MPD is
            // reloaded by the refresher, which merges these renditions too.
            int64_t period = 2000000;
            {
                std::lock_guard<std::mutex> lock(pres_mutex);
                if (!states.empty()) {
                    const Rendition &r = pres.renditions[static_cast<size_t>(states.front().choice.rendition)];
                    if (r.target_duration_us > 0) period = r.target_duration_us;
                    else if (!r.segments.empty() && r.segments.back().duration_us > 0) period = r.segments.back().duration_us;
                }
            }
            {
                std::unique_lock<std::mutex> lock(subs_mutex);
                subs_cv.wait_for(lock, std::chrono::microseconds(std::clamp<int64_t>(period, 500000, 10000000)),
                                 [&] { return subs_stop.load(); });
            }
            if (subs_stop) break;
            if (pres.format == Presentation::Format::Hls) {
                for (const SubState &st : states) {
                    std::string error;
                    if (!st.failed && !refresh_rendition(st.choice.rendition, Ctx::Subtitles, &error) && !subs_stop) {
                        WNK_LOGW(kCat, "subtitle refresh: %s", error.c_str());
                    }
                }
            }
        }
        for (const SubState &st : states) {
            const Rendition &r = pres.renditions[static_cast<size_t>(st.choice.rendition)];
            WNK_LOGI(kCat, "subtitles %s (%s): %zu cues from %zu segments", r.id.c_str(), r.language.c_str(), st.cues, st.next);
        }
    }

    // Starts loading every decodable subtitle track (the default first), once per open.
    void start_subtitles() {
        if (subs_thread.joinable()) return;
        std::vector<SubtitleChoice> todo;
        for (const SubtitleChoice &c : subtitle_choices) {
            if (c.kind != SubKind::None) todo.push_back(c);
        }
        if (todo.empty()) return;
        std::stable_partition(todo.begin(), todo.end(),
                              [&](const SubtitleChoice &c) { return info.tracks[static_cast<size_t>(c.track)].is_default; });
        subs_stop = false;
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            subs_cancel = std::make_shared<async::CancellationSource>();
        }
        {
            std::lock_guard<std::mutex> lock(subs_mutex);
            subs_running = true;
        }
        subs_thread = std::thread([this, todo, join = position_us] {
            load_subtitles(todo, join);
            std::lock_guard<std::mutex> lock(subs_mutex);
            subs_running = false;
            subs_cv.notify_all();
        });
    }

    void stop_subtitles() {
        {
            std::lock_guard<std::mutex> lock(subs_mutex);
            subs_stop = true;
            subs_cv.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(refresh_mutex);
            live_subtitles.clear();
        }
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            subs_cancel->cancel();
        }
        if (subs_thread.joinable()) subs_thread.join();
        std::lock_guard<std::mutex> lock(subs_mutex);
        cue_queue.clear();
        subs_running = false;
    }

    void build_info(const std::string &uri) {
        info = MediaInfo{};
        info.uri = uri;
        info.container = pres.format == Presentation::Format::Dash ? "dash" : "hls";
        info.duration_us = pres.live ? kNoTime : pres.duration_us;
        info.seekable = true;  // live: within the time-shift window
        const Rendition &v = pres.renditions[static_cast<size_t>(video)];
        TrackInfo vt;
        vt.index = 0;
        vt.type = TrackType::Video;
        vt.codec = video_codec(v);
        vt.codec_label = v.codecs.empty() ? "avc1" : codec_entry(v.codecs, TrackType::Video);
        vt.width = v.width;
        vt.height = v.height;
        vt.frame_rate = v.frame_rate;
        vt.is_default = true;
        vt.title = std::to_string(v.width) + "x" + std::to_string(v.height) + " " + vt.codec_label + " " + mbps(v.bandwidth);
        info.tracks.push_back(vt);

        audio_choices.clear();
        if (v.muxed_audio) {
            audio_choices.push_back({video, true});
        } else {
            // DASH: the best Representation of each AdaptationSet; HLS: every audio rendition.
            std::vector<int> picks;
            for (size_t i = 0; i < pres.renditions.size(); ++i) {
                const Rendition &r = pres.renditions[i];
                if (r.type != TrackType::Audio || r.period > 0 || !decryptable(r, opt.keys)) continue;
                const bool known = codec_from_rfc6381(r.codecs, TrackType::Audio) != CodecId::Unknown || r.codecs.empty();
                if (!known) continue;
                auto same = std::find_if(picks.begin(), picks.end(), [&](int k) {
                    const Rendition &o = pres.renditions[static_cast<size_t>(k)];
                    return pres.format == Presentation::Format::Dash ? o.group == r.group
                                                                     : o.playlist_url == r.playlist_url;
                });
                if (same == picks.end()) {
                    picks.push_back(static_cast<int>(i));
                } else if (pres.format == Presentation::Format::Dash
                               ? r.bandwidth > pres.renditions[static_cast<size_t>(*same)].bandwidth
                               : !r.group.empty()) {
                    // HLS: an audio-only variant on the same playlist as an EXT-X-MEDIA rendition
                    // says less (no language or name); keep the rendition.
                    *same = static_cast<int>(i);
                }
            }
            if (pres.format == Presentation::Format::Hls) {
                // The same audio in several groups (one per bitrate tier) is one choice: offer the
                // rendition of the video's group.
                std::vector<int> kept;
                for (int k : picks) {
                    bool covered = false;
                    for (int &c : kept) {
                        build_audio_ladder(c);
                        if (std::find(audio_ladder.begin(), audio_ladder.end(), k) == audio_ladder.end()) continue;
                        covered = true;
                        if (pres.renditions[static_cast<size_t>(k)].group == v.audio_group) c = k;
                        break;
                    }
                    if (!covered) kept.push_back(k);
                }
                audio_ladder.clear();
                picks = std::move(kept);
            }
            for (int k : picks) audio_choices.push_back({k, false});
        }
        // One default: the preferred language, else what the manifest marks, else the first.
        int def = -1;
        for (size_t i = 0; i < audio_choices.size() && def < 0; ++i) {
            if (language_matches(pres.renditions[static_cast<size_t>(audio_choices[i].rendition)].language, opt.language)) {
                def = static_cast<int>(i);
            }
        }
        for (size_t i = 0; i < audio_choices.size() && def < 0; ++i) {
            const auto &c = audio_choices[i];
            if (!c.muxed && pres.renditions[static_cast<size_t>(c.rendition)].is_default) def = static_cast<int>(i);
        }
        if (def < 0 && !audio_choices.empty()) def = 0;
        for (size_t i = 0; i < audio_choices.size(); ++i) {
            const auto &c = audio_choices[i];
            const Rendition &r = pres.renditions[static_cast<size_t>(c.rendition)];
            TrackInfo t;
            t.index = static_cast<int>(i) + 1;
            t.type = TrackType::Audio;
            t.codec = codec_from_rfc6381(r.codecs, TrackType::Audio);
            if (t.codec == CodecId::Unknown && r.codecs.empty()) t.codec = CodecId::Aac;  // HLS default
            t.codec_label = c.muxed ? codec_name(t.codec) : lower(codec_entry(r.codecs, TrackType::Audio));
            t.language = c.muxed ? "" : r.language;
            t.title = c.muxed ? "" : r.label;
            t.channels = c.muxed ? 0 : r.channels;
            t.sample_rate = c.muxed ? 0 : r.sample_rate;
            t.is_default = static_cast<int>(i) == def;
            t.decodable = t.codec != CodecId::Unknown;
            t.ac3_encodable = t.decodable && opt.ac3_encode;
            info.tracks.push_back(t);
        }

        // Subtitles: DASH, one per AdaptationSet; HLS, one per playlist. All are offered; those this
        // build cannot read as SubtitleKind::None.
        subtitle_choices.clear();
        for (size_t i = 0; i < pres.renditions.size(); ++i) {
            const Rendition &r = pres.renditions[i];
            if (r.type != TrackType::Subtitle || r.period > 0 || !decryptable(r, opt.keys)) continue;
            const bool seen = std::any_of(subtitle_choices.begin(), subtitle_choices.end(), [&](const SubtitleChoice &c) {
                const Rendition &o = pres.renditions[static_cast<size_t>(c.rendition)];
                return pres.format == Presentation::Format::Dash ? o.group == r.group : o.playlist_url == r.playlist_url;
            });
            if (seen) continue;
            SubtitleChoice c;
            c.rendition = static_cast<int>(i);
            c.track = static_cast<int>(info.tracks.size());
            c.kind = subtitle_kind_of(r);
            TrackInfo t;
            t.index = c.track;
            t.type = TrackType::Subtitle;
            t.codec_label = r.codecs.empty() ? (pres.format == Presentation::Format::Hls ? "webvtt" : lower(r.mime_type))
                                             : lower(r.codecs);
            t.language = r.language;
            t.title = r.label;
            t.is_default = r.is_default;
            t.subtitle = c.kind != SubKind::None ? SubtitleKind::Text : SubtitleKind::None;
            info.tracks.push_back(t);
            subtitle_choices.push_back(c);
        }
    }
};

AdaptiveSource::AdaptiveSource(AdaptiveSourceOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->opt = std::move(options);
    impl_->hls_keys->http = impl_->opt.http;
    impl_->hls_keys->headers = impl_->opt.headers;
    for (const auto &h : impl_->opt.license_headers) impl_->hls_keys->headers.push_back(h);
}

AdaptiveSource::~AdaptiveSource() {
    impl_->stop_subtitles();
    impl_->stop_refresh();
    impl_->close_lanes();
}

bool AdaptiveSource::handles(const std::string &uri) {
    if (uri.rfind("http://", 0) != 0 && uri.rfind("https://", 0) != 0) return false;
    const std::string path = lower(uri.substr(0, uri.find_first_of("?#")));
    if (ends_with(path, ".mpd") || ends_with(path, ".m3u8") || ends_with(path, ".m3u")) return true;
    // Provider scripts that answer with a playlist. A wrong guess fails in open() with a reason.
    if (ends_with(path, "play.php") || ends_with(path, "get.php")) return true;
    const size_t q = uri.find('?');
    const std::string query = q == std::string::npos ? std::string() : lower(uri.substr(q, uri.find('#', q) - q));
    for (const char *p : {"e=.m3u", "output=m3u8", "type=m3u8"}) {
        if (query.find(p) != std::string::npos) return true;
    }
    return false;
}

const MediaInfo &AdaptiveSource::info() const { return impl_->info; }
const Presentation &AdaptiveSource::presentation() const { return impl_->pres; }

int AdaptiveSource::video_rendition() const {
    for (const auto &l : impl_->lanes) {
        if (l.want_video) return l.rendition;
    }
    return -1;
}

int AdaptiveSource::audio_rendition() const { return impl_->sel_audio; }
void AdaptiveSource::set_bandwidth_limit(int64_t bps) { impl_->limit_bps = std::max<int64_t>(0, bps); }
int64_t AdaptiveSource::throughput_estimate() const { return impl_->estimator.estimate_bps(); }
int AdaptiveSource::switches() const { return impl_->switch_count.load(); }
int AdaptiveSource::audio_switches() const { return impl_->audio_switch_count.load(); }

bool AdaptiveSource::open(const std::string &uri, std::string *error) {
    Impl &m = *impl_;
    m.stop_subtitles();
    m.stop_refresh();
    m.close_lanes();
    m.live_ended = false;
    m.zero_us = 0;
    m.clock_offset_us = 0;
    m.window_first.clear();
    m.mpd_body.clear();
    m.mpd_url.clear();
    m.mpd_fetched_us = kNoTime;
    m.pres = Presentation{};
    m.info = MediaInfo{};
    m.video = -1;
    m.sel_audio = -1;
    m.position_us = 0;
    m.hls_origin_us = kNoTime;
    m.ladder.clear();
    m.active_ladder.clear();
    m.switch_count = 0;
    m.audio_switch_count = 0;
    std::string body, final_url;
    if (!m.fetch(uri, nullptr, &body, &final_url, error)) return false;
    const size_t start = body.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    const bool hls = start != std::string::npos && body.compare(start, 7, "#EXTM3U") == 0;
    if (hls && body.find("\n#EXT-X-") == std::string::npos) {
        *error = "an M3U channel list, not an HLS playlist: read it with parse_iptv_playlist()";
        return false;
    }
    if (!(hls ? parse_hls(body, final_url, &m.pres, error) : parse_mpd(body, final_url, &m.pres, error, m.now_us()))) {
        return false;
    }
    if (!hls && m.pres.live && !m.pres.utc_timing_scheme.empty()) {
        // A live MPD is read against the server's clock where it names one.
        m.sync_clock();
        if (m.clock_offset_us != 0 && !parse_mpd(body, final_url, &m.pres, error, m.now_us())) return false;
    }
    if (!hls && m.pres.live) {
        m.mpd_body = body;
        m.mpd_url = final_url;
        m.mpd_fetched_us = Impl::steady_us();
    }
    m.resolve_keys();
    m.video = choose_video(m.pres, m.opt);
    if (m.video < 0) {
        const bool encrypted = std::any_of(m.pres.renditions.begin(), m.pres.renditions.end(),
                                           [](const Rendition &r) { return r.type == TrackType::Video && r.encrypted; });
        if (encrypted && !m.key_error.empty()) {
            *error = "the video is encrypted: " + m.key_error;
        } else {
            *error = encrypted ? "the video is encrypted and no key for it was given (only ClearKey is supported)"
                               : "no video rendition this device can play";
        }
        return false;
    }
    m.build_ladder(m.video);
    if (m.opt.adaptive_bitrate && m.ladder.size() > 1) {
        // Start at the best rung the assumed bandwidth carries; measurements take it from there.
        int start = m.ladder.front();
        for (int k : m.ladder) {
            if (m.pres.renditions[static_cast<size_t>(k)].bandwidth <= m.opt.initial_bandwidth) start = k;
        }
        m.video = start;
    }
    if (!hls && m.pres.live) {
        // The timeline starts at the window's first segment.
        const auto &segs = m.pres.renditions[static_cast<size_t>(m.video)].segments;
        if (segs.empty()) {
            *error = "the live presentation has no segments yet";
            return false;
        }
        m.zero_us = segs.front().start_us;
        m.normalize(m.pres);
        for (size_t i = 0; i < m.pres.renditions.size(); ++i) {
            if (!m.pres.renditions[i].segments.empty()) m.window_first[static_cast<long>(i)] = m.pres.renditions[i].segments.front().number;
        }
    }
    Rendition &v = m.pres.renditions[static_cast<size_t>(m.video)];
    if (!m.ensure_segments(v, error)) return false;
    if (m.pres.live) {
        // Join a few segments behind the newest (HLS: three target durations; DASH: the suggested
        // presentation delay, else three segments), which leaves room for the next refresh. The
        // newest and the segment length are those of every rendition that may play (audio
        // segments can be longer than video ones and end earlier), and a DASH presentation delay
        // never puts the join point within two segments of that edge: a segment is only published
        // once complete, so the next one would not be there in time.
        int64_t seg = v.target_duration_us;
        int64_t edge = v.segments.back().start_us + v.segments.back().duration_us;
        auto consider = [&](const Rendition &r) {
            if (r.segments.empty()) return;
            for (const MediaSegment &s : r.segments) seg = std::max(seg, s.duration_us);
            edge = std::min(edge, r.segments.back().start_us + r.segments.back().duration_us);
        };
        consider(v);
        if (!hls) {
            for (const Rendition &r : m.pres.renditions) {
                if (r.type == TrackType::Audio && r.period == v.period) consider(r);
            }
        }
        const int64_t delay = !hls && m.pres.live_delay_us != kNoTime ? std::max(m.pres.live_delay_us, 2 * seg) : 3 * seg;
        m.position_us = std::max(v.segments.front().start_us, edge - delay);
        m.pres.duration_us = kNoTime;
        WNK_LOGI(kCat, "live: %zu segments listed, joining at %.1f s of %.1f s (segments up to %.1f s)", v.segments.size(),
                 m.position_us / 1e6, edge / 1e6, seg / 1e6);
    } else if (m.pres.duration_us == kNoTime) {
        int64_t total = 0;
        for (const MediaSegment &s : v.segments) total = std::max(total, s.start_us + s.duration_us);
        m.pres.duration_us = total;
    }
    m.build_info(uri);
    WNK_LOGI(kCat, "%s: %s, %zu renditions; video %s (%s), %zu audio choices", uri.c_str(), m.info.container.c_str(),
             m.pres.renditions.size(), v.id.c_str(), m.info.tracks[0].title.c_str(), m.audio_choices.size());
    return true;
}

bool AdaptiveSource::select(int video_track, int audio_track, const BackendCaps &caps, StreamFormat *out,
                            std::string *error) {
    Impl &m = *impl_;
    *out = StreamFormat{};
    if (m.video < 0) {
        *error = "not open";
        return false;
    }
    m.close_lanes();
    m.caps = caps;
    m.sel_audio = -1;
    m.active_ladder.clear();
    if (video_track == 0) {
        const Rendition &cur = m.pres.renditions[static_cast<size_t>(m.video)];
        for (int k : m.ladder) {
            const Rendition &r = m.pres.renditions[static_cast<size_t>(k)];
            if (caps.adaptive_resolution || (r.width == cur.width && r.height == cur.height)) m.active_ladder.push_back(k);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m.refresh_mutex);
        m.live_ladder.assign(m.active_ladder.begin(), m.active_ladder.end());
    }
    m.decided_samples[0] = m.decided_samples[1] = 0;
    m.decided_limit[0] = m.decided_limit[1] = 0;
    m.audio_ladder.clear();
    m.follow_tried = -1;
    if (video_track > 0 || audio_track == 0 ||
        (audio_track > 0 && static_cast<size_t>(audio_track - 1) >= m.audio_choices.size())) {
        *error = "no such track";
        return false;
    }
    std::vector<Impl::Lane> lanes;
    Impl::Lane main;
    main.rendition = m.video;
    main.head = m.video;
    main.want_video = video_track == 0;
    std::optional<Impl::AudioChoice> audio;
    if (audio_track > 0) audio = m.audio_choices[static_cast<size_t>(audio_track - 1)];
    if (audio && audio->muxed) main.want_audio = true;
    const bool with_video = main.want_video;
    if (main.want_video || main.want_audio) lanes.push_back(std::move(main));
    if (audio && !audio->muxed) {
        Rendition &r = m.pres.renditions[static_cast<size_t>(audio->rendition)];
        if (!m.ensure_segments(r, error)) return false;
        Impl::Lane aux;
        aux.rendition = audio->rendition;
        if (m.pres.format == Presentation::Format::Hls) {
            // The audio of the video's own variant (its group); with bitrate switching it follows the video.
            m.build_audio_ladder(audio->rendition);
            if (with_video) aux.rendition = m.variant_audio(m.video, audio->rendition);
            if (!m.opt.adaptive_bitrate || !with_video) m.audio_ladder.clear();
            Rendition &sr = m.pres.renditions[static_cast<size_t>(aux.rendition)];
            if (!m.ensure_segments(sr, error)) return false;
        } else if (m.opt.adaptive_bitrate) {
            m.build_audio_ladder(audio->rendition);
            if (m.audio_ladder.size() > 1) {
                // Start at the best rung that fits beside the video in the assumed bandwidth.
                const int64_t video_bw = with_video ? m.pres.renditions[static_cast<size_t>(m.video)].bandwidth : 0;
                const int64_t budget = m.opt.initial_bandwidth - video_bw;
                int start = m.audio_ladder.front();
                for (int k : m.audio_ladder) {
                    if (m.pres.renditions[static_cast<size_t>(k)].bandwidth <= budget) start = k;
                }
                aux.rendition = start;
                Rendition &sr = m.pres.renditions[static_cast<size_t>(start)];
                if (!m.ensure_segments(sr, error)) return false;
            }
        }
        aux.head = aux.rendition;
        aux.want_audio = true;
        m.sel_audio = aux.rendition;
        lanes.push_back(std::move(aux));
        if (m.audio_ladder.size() > 1) {
            std::lock_guard<std::mutex> lock(m.refresh_mutex);  // live: the refresher keeps them listed
            m.live_ladder.insert(m.live_ladder.end(), m.audio_ladder.begin(), m.audio_ladder.end());
        }
    }
    if (lanes.empty()) {
        *error = "nothing selected";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m.lanes_mutex);
        m.lanes = std::move(lanes);
    }
    if (!m.open_at(m.position_us, out, error)) {
        m.close_lanes();
        return false;
    }
    m.start_subtitles();  // after the video is open: HLS cue times need its timeline origin
    m.start_refresh();    // live: follow what the server publishes
    return out->video || out->audio;
}

MediaSource::Read AdaptiveSource::read(Packet *out, std::string *error) {
    Impl &m = *impl_;
    for (;;) {
        if (m.interrupted) return Read::Interrupted;
        if (m.reopen) {
            StreamFormat ignored;
            if (!m.open_at(m.position_us, &ignored, error)) {
                if (m.interrupted) return Read::Interrupted;
                return Read::Error;
            }
            m.reopen = false;
        }
        {
            // Subtitle cues from the loader go first: they are small and the controller stores them.
            std::lock_guard<std::mutex> lock(m.subs_mutex);
            if (!m.cue_queue.empty()) {
                Packet p;
                p.type = TrackType::Subtitle;
                p.key = true;
                p.subtitle = std::move(m.cue_queue.front());
                p.pts_us = p.subtitle->start_us;
                m.cue_queue.pop_front();
                *out = std::move(p);
                return Read::Packet;
            }
        }
        m.maybe_switch();
        for (Impl::Lane &lane : m.lanes) {
            const Read r = m.pull(lane, error);
            if (r != Read::Packet) return r;
        }
        // Interleave by decode time, so B-frame video does not hold back audio.
        auto when = [](const Packet &p) { return p.dts_us != kNoTime ? p.dts_us : p.pts_us; };
        Impl::Lane *next = nullptr;
        for (Impl::Lane &lane : m.lanes) {
            if (!lane.pending) continue;
            if (next == nullptr || when(*lane.pending) == kNoTime ||
                (when(*next->pending) != kNoTime && when(*lane.pending) < when(*next->pending))) {
                next = &lane;
            }
        }
        if (next == nullptr) {
            // The media is over; cues still being fetched come first (a short title read fast).
            std::unique_lock<std::mutex> lock(m.subs_mutex);
            m.subs_cv.wait_for(lock, std::chrono::milliseconds(100),
                               [&] { return !m.cue_queue.empty() || !m.subs_running || m.interrupted; });
            if (!m.cue_queue.empty() || (m.subs_running && !m.interrupted)) continue;
            return Read::EndOfStream;
        }
        *out = std::move(*next->pending);
        next->pending.reset();
        return Read::Packet;
    }
}

bool AdaptiveSource::seek(int64_t pts_us, std::string *error) {
    Impl &m = *impl_;
    int64_t t = std::max<int64_t>(0, pts_us);
    if (m.pres.live && m.video >= 0) {
        // Live: within what the server still lists, and not past the newest segment.
        std::lock_guard<std::mutex> lock(m.pres_mutex);
        const auto &segs = m.pres.renditions[static_cast<size_t>(m.video)].segments;
        if (!segs.empty()) {
            int64_t lo = segs.front().start_us;
            const auto wf = m.window_first.find(m.video);
            if (wf != m.window_first.end()) {
                for (const MediaSegment &s : segs) {
                    if (s.number == wf->second) {
                        lo = s.start_us;
                        break;
                    }
                }
            }
            t = std::clamp(t, lo, std::max(lo, segs.back().start_us));
        }
    }
    if (m.lanes.empty()) {
        m.position_us = t;
        return true;
    }
    StreamFormat ignored;
    m.reopen = false;
    if (!m.open_at(t, &ignored, error)) {
        if (!m.interrupted) return false;
        // Interrupted by a newer command (a held arrow key seeks again): like a demuxer seek that
        // was cut short, this one succeeds and read() reports the interrupt; reading on without a
        // new seek reopens here.
        m.position_us = t;
        m.reopen = true;
    }
    return true;
}

void AdaptiveSource::interrupt() {
    Impl &m = *impl_;
    m.interrupted = true;
    {
        std::lock_guard<std::mutex> lock(m.fetch_mutex);
        m.fetch_cancel->cancel();
    }
    std::lock_guard<std::mutex> lock(m.lanes_mutex);
    for (auto &l : m.lanes) {
        if (l.src) l.src->interrupt();
    }
}

void AdaptiveSource::reset_interrupt() {
    Impl &m = *impl_;
    m.interrupted = false;
    {
        std::lock_guard<std::mutex> lock(m.fetch_mutex);
        m.fetch_cancel = std::make_shared<async::CancellationSource>();
    }
    std::lock_guard<std::mutex> lock(m.lanes_mutex);
    for (auto &l : m.lanes) {
        if (l.src) l.src->reset_interrupt();
    }
}

}  // namespace wnk::media
