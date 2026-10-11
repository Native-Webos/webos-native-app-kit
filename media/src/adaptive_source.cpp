// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/adaptive_source.hpp"

#include "wnk/log.hpp"
#include "wnk/media/ffmpeg_source.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
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

}  // namespace

int choose_video(const Presentation &p, const AdaptiveSourceOptions &o) {
    bool any_plain = false;
    for (const Rendition &r : p.renditions) {
        if (r.type == TrackType::Video && !r.encrypted && r.dolby_vision.empty()) any_plain = true;
    }
    int best = -1;
    auto rank = [&](const Rendition &r) {
        const auto it = std::find(o.video_codecs.begin(), o.video_codecs.end(), video_codec(r));
        return static_cast<int>(it - o.video_codecs.begin());
    };
    for (size_t i = 0; i < p.renditions.size(); ++i) {
        const Rendition &r = p.renditions[i];
        if (r.type != TrackType::Video || r.encrypted || is_webm(r)) continue;
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
    int video = -1;                          // chosen video rendition

    // One FfmpegSource reading one rendition.
    struct Lane {
        int rendition = -1;
        bool want_video = false, want_audio = false;
        std::unique_ptr<FfmpegSource> src;
        std::optional<Packet> pending;
        bool ended = false;
        // Packets ending before this are dropped: an audio lane reopened at its own segment boundary
        // may start well before the video's keyframe (segment lengths differ, and HLS EXTINF values
        // are only approximate).
        int64_t drop_before_us = kNoTime;
    };
    std::mutex lanes_mutex;  // guards the lane pointers against interrupt() from other threads
    std::vector<Lane> lanes;
    BackendCaps caps;
    int sel_audio = -1;        // rendition of a separate audio lane
    int64_t position_us = 0;   // where the next select() starts
    bool reopen = false;       // a seek was interrupted while reopening: reopen at position_us first
    int64_t hls_origin_us = kNoTime;
    std::atomic<bool> interrupted{false};
    std::mutex fetch_mutex;
    std::shared_ptr<async::CancellationSource> fetch_cancel = std::make_shared<async::CancellationSource>();

    // GET (optionally a byte range), with retries on transient failures.
    bool fetch(const std::string &url, const ByteRange *range, std::string *body, std::string *final_url,
               std::string *error) {
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
            token = fetch_cancel->token();
        }
        const net::RetryPolicy retry;
        for (int attempt = 1;; ++attempt) {
            if (interrupted) {
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
            if (interrupted || r.error == net::NetError::Cancelled) {
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
    bool ensure_segments(Rendition &r, std::string *error) {
        if (r.needs_playlist()) {
            std::string body, final_url;
            if (!fetch(r.playlist_url, nullptr, &body, &final_url, error)) return false;
            bool live = false;
            if (!parse_hls_media(body, final_url, &r, &live, error)) return false;
            if (live) {
                *error = "live HLS playlists are not supported yet";
                r.segments.clear();
                return false;
            }
        } else if (r.needs_index()) {
            std::string body;
            if (!fetch(r.index_url, &r.index, &body, nullptr, error)) return false;
            std::vector<MediaSegment> segs;
            if (!parse_sidx(reinterpret_cast<const uint8_t *>(body.data()), body.size(), r.index.offset, r.index_url,
                            &segs, error)) {
                return false;
            }
            const int64_t origin = r.time_origin_us != kNoTime ? r.time_origin_us : 0;
            for (MediaSegment &s : segs) s.start_us -= origin;
            r.segments = std::move(segs);
        }
        if (r.segments.empty()) {
            *error = "rendition " + r.id + " has no segments";
            return false;
        }
        return true;
    }

    std::vector<net::SegmentPart> parts_from(const Rendition &r, size_t first) const {
        std::vector<net::SegmentPart> parts;
        if (r.init) parts.push_back({r.init->url, r.init->range.offset, r.init->range.length});
        for (size_t i = first; i < r.segments.size(); ++i) {
            const MediaSegment &s = r.segments[i];
            parts.push_back({s.url, s.range.offset, s.range.length});
        }
        return parts;
    }

    // Opens `lane` on its rendition from segment `first`, delivering what it wants; `fmt` gets what
    // the inner source will deliver.
    bool open_lane(Lane &lane, size_t first, StreamFormat *fmt, std::string *error) {
        const Rendition &r = pres.renditions[static_cast<size_t>(lane.rendition)];
        const bool hls = pres.format == Presentation::Format::Hls;
        for (int pass = 0; pass < 2; ++pass) {
            net::SegmentReaderOptions so = opt.segments;
            so.headers = opt.headers;
            auto reader = std::make_unique<net::SegmentReader>(opt.http, parts_from(r, first), so);
            auto src = std::make_unique<FfmpegSource>(FfmpegSourceOptions{nullptr, {}, opt.ac3_encode});
            src->set_time_origin(hls ? hls_origin_us : r.time_origin_us);
            FfmpegSource *s = src.get();
            {
                std::lock_guard<std::mutex> lock(lanes_mutex);
                lane.src = std::move(src);
                lane.pending.reset();
                lane.ended = false;
                if (interrupted) s->interrupt();
            }
            if (!s->open_reader(std::move(reader), r.segments[first].url, error)) {
                *error = "rendition " + r.id + ": " + *error;
                return false;
            }
            if (hls && hls_origin_us == kNoTime) {
                // All HLS renditions share the timeline of the video's first segment.
                hls_origin_us = s->time_origin() - r.segments[first].start_us;
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
            if (r == MediaSource::Read::EndOfStream) {
                lane.ended = true;
            } else if (r != MediaSource::Read::Packet) {
                return r;
            } else if (p.type != TrackType::Subtitle) {
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

    // (Re)opens the selected renditions at `t`.
    bool open_at(int64_t t, StreamFormat *out, std::string *error) {
        if (lanes.empty()) return false;
        int64_t main_start = t;
        int64_t main_first_pts = kNoTime;
        for (size_t i = 0; i < lanes.size(); ++i) {
            Lane &lane = lanes[i];
            const auto &segs = pres.renditions[static_cast<size_t>(lane.rendition)].segments;
            // The first lane leads (video if any); the others start at or before its segment.
            const size_t first = i == 0 ? segment_at(segs, t) : segment_at(segs, main_start);
            if (i == 0) main_start = segs[first].start_us;
            StreamFormat fmt;
            if (!open_lane(lane, first, &fmt, error)) return false;
            if (out != nullptr) {
                if (fmt.video) out->video = fmt.video;
                if (fmt.audio) out->audio = fmt.audio;
            }
            if (i == 0 && lane.want_video) {
                // Where the video really starts (its keyframe), for trimming the other lanes.
                const MediaSource::Read r = pull(lane, error);
                if (r != MediaSource::Read::Packet) {
                    if (r == MediaSource::Read::Interrupted) *error = "interrupted";
                    return false;
                }
                if (lane.pending) main_first_pts = lane.pending->pts_us;
            } else if (i > 0) {
                lane.drop_before_us = main_first_pts;
            }
        }
        position_us = t;
        return true;
    }

    void build_info(const std::string &uri) {
        info = MediaInfo{};
        info.uri = uri;
        info.container = pres.format == Presentation::Format::Dash ? "dash" : "hls";
        info.duration_us = pres.duration_us;
        info.seekable = true;
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
                if (r.type != TrackType::Audio || r.encrypted || is_webm(r)) continue;
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
    }
};

AdaptiveSource::AdaptiveSource(AdaptiveSourceOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->opt = std::move(options);
}

AdaptiveSource::~AdaptiveSource() { impl_->close_lanes(); }

bool AdaptiveSource::handles(const std::string &uri) {
    if (uri.rfind("http://", 0) != 0 && uri.rfind("https://", 0) != 0) return false;
    const std::string path = lower(uri.substr(0, uri.find_first_of("?#")));
    return ends_with(path, ".mpd") || ends_with(path, ".m3u8");
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

bool AdaptiveSource::open(const std::string &uri, std::string *error) {
    Impl &m = *impl_;
    m.close_lanes();
    m.pres = Presentation{};
    m.info = MediaInfo{};
    m.video = -1;
    m.sel_audio = -1;
    m.position_us = 0;
    m.hls_origin_us = kNoTime;
    std::string body, final_url;
    if (!m.fetch(uri, nullptr, &body, &final_url, error)) return false;
    const size_t start = body.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    const bool hls = start != std::string::npos && body.compare(start, 7, "#EXTM3U") == 0;
    if (!(hls ? parse_hls(body, final_url, &m.pres, error) : parse_mpd(body, final_url, &m.pres, error))) return false;
    if (m.pres.live) {
        *error = "live presentations are not supported yet";
        return false;
    }
    m.video = choose_video(m.pres, m.opt);
    if (m.video < 0) {
        const bool encrypted = std::any_of(m.pres.renditions.begin(), m.pres.renditions.end(),
                                           [](const Rendition &r) { return r.type == TrackType::Video && r.encrypted; });
        *error = encrypted ? "the video is encrypted (DRM is not supported)" : "no video rendition this device can play";
        return false;
    }
    Rendition &v = m.pres.renditions[static_cast<size_t>(m.video)];
    if (!m.ensure_segments(v, error)) return false;
    if (m.pres.duration_us == kNoTime) {
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
    if (video_track > 0 || audio_track == 0 ||
        (audio_track > 0 && static_cast<size_t>(audio_track - 1) >= m.audio_choices.size())) {
        *error = "no such track";
        return false;
    }
    std::vector<Impl::Lane> lanes;
    Impl::Lane main;
    main.rendition = m.video;
    main.want_video = video_track == 0;
    std::optional<Impl::AudioChoice> audio;
    if (audio_track > 0) audio = m.audio_choices[static_cast<size_t>(audio_track - 1)];
    if (audio && audio->muxed) main.want_audio = true;
    if (main.want_video || main.want_audio) lanes.push_back(std::move(main));
    if (audio && !audio->muxed) {
        Rendition &r = m.pres.renditions[static_cast<size_t>(audio->rendition)];
        if (!m.ensure_segments(r, error)) return false;
        Impl::Lane aux;
        aux.rendition = audio->rendition;
        aux.want_audio = true;
        lanes.push_back(std::move(aux));
        m.sel_audio = audio->rendition;
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
        if (next == nullptr) return Read::EndOfStream;
        *out = std::move(*next->pending);
        next->pending.reset();
        return Read::Packet;
    }
}

bool AdaptiveSource::seek(int64_t pts_us, std::string *error) {
    Impl &m = *impl_;
    const int64_t t = std::max<int64_t>(0, pts_us);
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
