// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/ffmpeg_source.hpp"

#include "wnk/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <deque>

namespace wnk::media {
namespace {

constexpr const char *kCat = "media.ff";
constexpr AVRational kMicros{1, 1000000};

std::string av_err(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

CodecId map_codec(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return CodecId::H264;
        case AV_CODEC_ID_HEVC: return CodecId::Hevc;
        case AV_CODEC_ID_VP9: return CodecId::Vp9;
        case AV_CODEC_ID_AV1: return CodecId::Av1;
        case AV_CODEC_ID_AAC: return CodecId::Aac;
        case AV_CODEC_ID_AC3: return CodecId::Ac3;
        case AV_CODEC_ID_EAC3: return CodecId::Eac3;
        case AV_CODEC_ID_MP3: return CodecId::Mp3;
        case AV_CODEC_ID_OPUS: return CodecId::Opus;
        case AV_CODEC_ID_PCM_S16LE: return CodecId::PcmS16;
        default: return CodecId::Unknown;
    }
}

std::string meta(AVDictionary *d, const char *key) {
    const AVDictionaryEntry *e = av_dict_get(d, key, nullptr, 0);
    return e != nullptr && e->value != nullptr ? e->value : "";
}

// FFmpeg's own messages go to the kit log (warnings and worse), not to stderr.
void log_callback(void *avcl, int level, const char *fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;
    char line[512];
    int prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof line, &prefix);
    size_t n = std::strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (n == 0) return;
    if (level <= AV_LOG_ERROR) {
        WNK_LOGE("ffmpeg", "%s", line);
    } else {
        WNK_LOGW("ffmpeg", "%s", line);
    }
}

int16_t clip16(float v) {
    const float s = v * 32767.0f;
    return static_cast<int16_t>(std::lrintf(std::clamp(s, -32768.0f, 32767.0f)));
}

}  // namespace

void mix_to_stereo_s16(const float *const *planes, int channels, int samples, const int *order, int16_t *out) {
    // Weight of each input channel in the left and right outputs.
    float wl[16] = {}, wr[16] = {};
    const int n = std::min(channels, 16);
    if (n == 1) {
        wl[0] = wr[0] = 1.0f;
    } else {
        for (int c = 0; c < n; ++c) {
            const int pos = order != nullptr ? order[c] : c;
            switch (pos) {
                case AV_CHAN_FRONT_LEFT: wl[c] = 1.0f; break;
                case AV_CHAN_FRONT_RIGHT: wr[c] = 1.0f; break;
                case AV_CHAN_FRONT_CENTER: wl[c] = wr[c] = 0.7071f; break;
                case AV_CHAN_BACK_LEFT:
                case AV_CHAN_SIDE_LEFT: wl[c] = 0.7071f; break;
                case AV_CHAN_BACK_RIGHT:
                case AV_CHAN_SIDE_RIGHT: wr[c] = 0.7071f; break;
                default: break;  // LFE and anything exotic are dropped
            }
        }
    }
    // Normalise so a full-scale signal on every channel cannot clip.
    float sum_l = 0, sum_r = 0;
    for (int c = 0; c < n; ++c) {
        sum_l += wl[c];
        sum_r += wr[c];
    }
    const float norm = 1.0f / std::max(1.0f, std::max(sum_l, sum_r));
    for (int i = 0; i < samples; ++i) {
        float l = 0, r = 0;
        for (int c = 0; c < n; ++c) {
            const float v = planes[c][i];
            l += v * wl[c];
            r += v * wr[c];
        }
        out[2 * i] = clip16(l * norm);
        out[2 * i + 1] = clip16(r * norm);
    }
}

struct FfmpegSource::Impl {
    AVFormatContext *fmt = nullptr;
    AVBSFContext *bsf = nullptr;
    AVCodecContext *dec = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    MediaInfo info;
    int video = -1, audio = -1;
    bool decode_audio = false;
    int64_t start_us = 0;  // subtracted from every timestamp
    bool need_key = false;  // after a seek: drop video until a keyframe
    bool eof = false;
    std::deque<Packet> ready;
    std::atomic<bool> interrupted{false};
    std::vector<float> planar_tmp;

    ~Impl() { close(); }

    void close() {
        av_bsf_free(&bsf);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        ready.clear();
        info = MediaInfo{};
        video = audio = -1;
        decode_audio = false;
        eof = false;
    }

    static int interrupt_cb(void *opaque) { return static_cast<Impl *>(opaque)->interrupted.load() ? 1 : 0; }

    int64_t to_us(int64_t ts, AVRational tb) const {
        if (ts == AV_NOPTS_VALUE) return kNoTime;
        return av_rescale_q(ts, tb, kMicros) - start_us;
    }

    // Converts one decoded audio frame to S16 stereo and queues it.
    void queue_pcm(const AVFrame *f, AVRational tb) {
        const int ch = f->ch_layout.nb_channels;
        const int ns = f->nb_samples;
        if (ch <= 0 || ns <= 0) return;
        std::vector<const float *> planes(static_cast<size_t>(ch));
        const auto fmt_id = static_cast<AVSampleFormat>(f->format);
        if (fmt_id == AV_SAMPLE_FMT_FLTP) {
            for (int c = 0; c < ch; ++c) planes[c] = reinterpret_cast<const float *>(f->extended_data[c]);
        } else {
            // Convert other layouts to planar float first.
            planar_tmp.assign(static_cast<size_t>(ch) * ns, 0.0f);
            for (int c = 0; c < ch; ++c) {
                float *dst = planar_tmp.data() + static_cast<size_t>(c) * ns;
                for (int i = 0; i < ns; ++i) {
                    switch (fmt_id) {
                        case AV_SAMPLE_FMT_FLT: dst[i] = reinterpret_cast<const float *>(f->data[0])[i * ch + c]; break;
                        case AV_SAMPLE_FMT_S16: dst[i] = reinterpret_cast<const int16_t *>(f->data[0])[i * ch + c] / 32768.0f; break;
                        case AV_SAMPLE_FMT_S16P: dst[i] = reinterpret_cast<const int16_t *>(f->extended_data[c])[i] / 32768.0f; break;
                        case AV_SAMPLE_FMT_S32: dst[i] = reinterpret_cast<const int32_t *>(f->data[0])[i * ch + c] / 2147483648.0f; break;
                        case AV_SAMPLE_FMT_S32P: dst[i] = reinterpret_cast<const int32_t *>(f->extended_data[c])[i] / 2147483648.0f; break;
                        default: dst[i] = 0; break;
                    }
                }
                planes[c] = dst;
            }
        }
        std::vector<int> order(static_cast<size_t>(ch));
        for (int c = 0; c < ch; ++c) {
            order[c] = f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE || f->ch_layout.order == AV_CHANNEL_ORDER_CUSTOM
                           ? static_cast<int>(av_channel_layout_channel_from_index(&f->ch_layout, static_cast<unsigned>(c)))
                           : c;
        }
        Packet p;
        p.type = TrackType::Audio;
        p.key = true;
        p.data.resize(static_cast<size_t>(ns) * 4);
        mix_to_stereo_s16(planes.data(), ch, ns, order.data(), reinterpret_cast<int16_t *>(p.data.data()));
        p.pts_us = to_us(f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts, tb);
        p.duration_us = static_cast<int64_t>(ns) * 1000000 / std::max(1, f->sample_rate);
        ready.push_back(std::move(p));
    }

    void drain_decoder(AVRational tb) {
        while (avcodec_receive_frame(dec, frame) == 0) {
            queue_pcm(frame, tb);
            av_frame_unref(frame);
        }
    }

    void drain_bsf(AVRational tb) {
        AVPacket *out = av_packet_alloc();
        while (av_bsf_receive_packet(bsf, out) == 0) {
            queue_video(out, tb);
            av_packet_unref(out);
        }
        av_packet_free(&out);
    }

    void queue_video(const AVPacket *p, AVRational tb) {
        const bool key = (p->flags & AV_PKT_FLAG_KEY) != 0;
        if (need_key && !key) return;
        need_key = false;
        Packet v;
        v.type = TrackType::Video;
        v.key = key;
        v.data.assign(p->data, p->data + p->size);
        v.pts_us = to_us(p->pts != AV_NOPTS_VALUE ? p->pts : p->dts, tb);
        v.duration_us = p->duration > 0 ? av_rescale_q(p->duration, tb, kMicros) : 0;
        ready.push_back(std::move(v));
    }
};

FfmpegSource::FfmpegSource() : impl_(std::make_unique<Impl>()) {
    static const bool logging = [] {
        av_log_set_callback(log_callback);
        return true;
    }();
    (void)logging;
}
FfmpegSource::~FfmpegSource() = default;

std::string FfmpegSource::version() {
    auto v = [](unsigned x) {
        return std::to_string(AV_VERSION_MAJOR(x)) + "." + std::to_string(AV_VERSION_MINOR(x)) + "." +
               std::to_string(AV_VERSION_MICRO(x));
    };
    return "avformat " + v(avformat_version()) + " / avcodec " + v(avcodec_version()) + " / avutil " + v(avutil_version());
}

const MediaInfo &FfmpegSource::info() const { return impl_->info; }
void FfmpegSource::interrupt() { impl_->interrupted = true; }
void FfmpegSource::reset_interrupt() { impl_->interrupted = false; }

bool FfmpegSource::open(const std::string &uri, std::string *error) {
    Impl &m = *impl_;
    m.close();
    std::string path = uri;
    if (path.rfind("file://", 0) == 0) path = path.substr(7);
    if (path.empty() || path[0] != '/') {
        *error = "only local files (file:// or absolute paths) are supported";
        return false;
    }
    m.fmt = avformat_alloc_context();
    m.fmt->interrupt_callback = {&Impl::interrupt_cb, &m};
    int rc = avformat_open_input(&m.fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        // avformat_open_input frees the context on failure.
        *error = rc == AVERROR_EXIT ? "interrupted" : av_err(rc);
        return false;
    }
    rc = avformat_find_stream_info(m.fmt, nullptr);
    if (rc < 0) {
        *error = rc == AVERROR_EXIT ? "interrupted" : "stream info: " + av_err(rc);
        m.close();
        return false;
    }
    m.pkt = av_packet_alloc();
    m.frame = av_frame_alloc();
    m.start_us = m.fmt->start_time != AV_NOPTS_VALUE ? m.fmt->start_time : 0;
    MediaInfo &info = m.info;
    info.uri = uri;
    info.container = m.fmt->iformat != nullptr ? m.fmt->iformat->name : "";
    info.duration_us = m.fmt->duration != AV_NOPTS_VALUE ? m.fmt->duration : kNoTime;
    info.seekable = m.fmt->pb != nullptr && (m.fmt->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
    for (unsigned i = 0; i < m.fmt->nb_streams; ++i) {
        const AVStream *st = m.fmt->streams[i];
        const AVCodecParameters *par = st->codecpar;
        TrackInfo t;
        t.index = static_cast<int>(i);
        t.codec = map_codec(par->codec_id);
        t.codec_label = avcodec_get_name(par->codec_id);
        t.language = meta(st->metadata, "language");
        t.title = meta(st->metadata, "title");
        t.is_default = (st->disposition & AV_DISPOSITION_DEFAULT) != 0;
        switch (par->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) continue;  // cover art
                t.type = TrackType::Video;
                t.width = par->width;
                t.height = par->height;
                t.frame_rate = st->avg_frame_rate.den != 0 ? av_q2d(st->avg_frame_rate) : 0;
                break;
            case AVMEDIA_TYPE_AUDIO:
                t.type = TrackType::Audio;
                t.sample_rate = par->sample_rate;
                t.channels = par->ch_layout.nb_channels;
                break;
            case AVMEDIA_TYPE_SUBTITLE: t.type = TrackType::Subtitle; break;
            default: t.type = TrackType::Other; break;
        }
        info.tracks.push_back(t);
    }
    return true;
}

bool FfmpegSource::select(int video_track, int audio_track, const BackendCaps &caps, StreamFormat *out,
                          std::string *error) {
    Impl &m = *impl_;
    if (m.fmt == nullptr) {
        *error = "not open";
        return false;
    }
    av_bsf_free(&m.bsf);
    avcodec_free_context(&m.dec);
    m.video = m.audio = -1;
    m.decode_audio = false;
    *out = StreamFormat{};

    if (video_track >= 0) {
        const TrackInfo *t = m.info.track(video_track);
        if (t == nullptr || t->type != TrackType::Video) {
            *error = "no video track " + std::to_string(video_track);
            return false;
        }
        if (!caps.accepts(t->codec)) {
            *error = "video codec " + t->codec_label + " is not supported by the decoder";
            return false;
        }
        const AVStream *st = m.fmt->streams[video_track];
        const AVCodecParameters *par = st->codecpar;
        // Length-prefixed (avcC/hvcC extradata starts with version 1) needs conversion to Annex-B.
        const bool length_prefixed = par->extradata_size > 0 && par->extradata[0] == 1;
        const char *filter = nullptr;
        if (length_prefixed && t->codec == CodecId::H264) filter = "h264_mp4toannexb";
        if (length_prefixed && t->codec == CodecId::Hevc) filter = "hevc_mp4toannexb";
        if (filter != nullptr) {
            const AVBitStreamFilter *f = av_bsf_get_by_name(filter);
            if (f == nullptr || av_bsf_alloc(f, &m.bsf) < 0) {
                *error = std::string("bitstream filter ") + filter + " unavailable";
                return false;
            }
            avcodec_parameters_copy(m.bsf->par_in, par);
            m.bsf->time_base_in = st->time_base;
            if (av_bsf_init(m.bsf) < 0) {
                *error = std::string("bitstream filter ") + filter + " failed to start";
                av_bsf_free(&m.bsf);
                return false;
            }
        }
        m.video = video_track;
        out->video = VideoFormat{t->codec, t->width, t->height, t->frame_rate};
    }

    if (audio_track >= 0) {
        const TrackInfo *t = m.info.track(audio_track);
        if (t == nullptr || t->type != TrackType::Audio) {
            *error = "no audio track " + std::to_string(audio_track);
            return false;
        }
        const AVCodecParameters *par = m.fmt->streams[audio_track]->codecpar;
        if (caps.accepts(t->codec)) {
            AudioFormat a{t->codec, t->sample_rate, t->channels, {}};
            if (par->extradata_size > 0) a.codec_header.assign(par->extradata, par->extradata + par->extradata_size);
            out->audio = a;
            m.audio = audio_track;
        } else if (caps.accepts(CodecId::PcmS16) &&
                   (t->codec == CodecId::Aac || t->codec == CodecId::Ac3 || t->codec == CodecId::Eac3)) {
            const AVCodec *codec = avcodec_find_decoder(par->codec_id);
            m.dec = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
            if (m.dec == nullptr || avcodec_parameters_to_context(m.dec, par) < 0) {
                *error = "no decoder for " + t->codec_label;
                avcodec_free_context(&m.dec);
                return false;
            }
            m.dec->pkt_timebase = m.fmt->streams[audio_track]->time_base;
            // AC-3/E-AC-3 can downmix inside the decoder, which also applies the stream's mix levels.
            if (t->codec != CodecId::Aac) {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                av_opt_set_chlayout(m.dec->priv_data, "downmix", &stereo, 0);
            }
            if (avcodec_open2(m.dec, codec, nullptr) < 0) {
                *error = "could not open the " + t->codec_label + " decoder";
                avcodec_free_context(&m.dec);
                return false;
            }
            m.decode_audio = true;
            m.audio = audio_track;
            out->audio = AudioFormat{CodecId::PcmS16, t->sample_rate, 2, {}};
        } else {
            // Play the picture without sound rather than refusing the file.
            WNK_LOGW(kCat, "audio codec %s cannot be played here; video only", t->codec_label.c_str());
        }
    }
    m.need_key = true;
    return out->video || out->audio;
}

FfmpegSource::Read FfmpegSource::read(Packet *out, std::string *error) {
    Impl &m = *impl_;
    for (;;) {
        if (!m.ready.empty()) {
            *out = std::move(m.ready.front());
            m.ready.pop_front();
            return Read::Packet;
        }
        if (m.interrupted) return Read::Interrupted;
        if (m.eof) return Read::EndOfStream;
        const int rc = av_read_frame(m.fmt, m.pkt);
        if (rc == AVERROR_EOF) {
            // Drain what the filter and decoder still hold.
            m.eof = true;
            if (m.bsf != nullptr) {
                av_bsf_send_packet(m.bsf, nullptr);
                m.drain_bsf(m.fmt->streams[m.video]->time_base);
            }
            if (m.dec != nullptr) {
                avcodec_send_packet(m.dec, nullptr);
                m.drain_decoder(m.fmt->streams[m.audio]->time_base);
            }
            continue;
        }
        if (rc < 0) {
            if (rc == AVERROR_EXIT) return Read::Interrupted;
            *error = av_err(rc);
            return Read::Error;
        }
        const int si = m.pkt->stream_index;
        const AVRational tb = m.fmt->streams[si]->time_base;
        if (si == m.video) {
            if (m.bsf != nullptr) {
                if (av_bsf_send_packet(m.bsf, m.pkt) == 0) m.drain_bsf(tb);  // takes the packet's reference
            } else {
                m.queue_video(m.pkt, tb);
            }
        } else if (si == m.audio) {
            if (m.decode_audio) {
                if (avcodec_send_packet(m.dec, m.pkt) == 0) m.drain_decoder(tb);
            } else if (!m.need_key || m.video < 0) {
                Packet a;
                a.type = TrackType::Audio;
                a.key = true;
                a.data.assign(m.pkt->data, m.pkt->data + m.pkt->size);
                a.pts_us = m.to_us(m.pkt->pts != AV_NOPTS_VALUE ? m.pkt->pts : m.pkt->dts, tb);
                a.duration_us = m.pkt->duration > 0 ? av_rescale_q(m.pkt->duration, tb, kMicros) : 0;
                m.ready.push_back(std::move(a));
            }
        }
        av_packet_unref(m.pkt);
    }
}

bool FfmpegSource::seek(int64_t pts_us, std::string *error) {
    Impl &m = *impl_;
    if (m.fmt == nullptr) {
        *error = "not open";
        return false;
    }
    // Seek on the video stream when there is one, so the landing point is a video keyframe.
    const int stream = m.video >= 0 ? m.video : -1;
    const int64_t target_us = pts_us + m.start_us;
    const int64_t ts = stream >= 0 ? av_rescale_q(target_us, kMicros, m.fmt->streams[stream]->time_base) : target_us;
    const int rc = av_seek_frame(m.fmt, stream, ts, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        *error = av_err(rc);
        return false;
    }
    if (m.bsf != nullptr) av_bsf_flush(m.bsf);
    if (m.dec != nullptr) avcodec_flush_buffers(m.dec);
    m.ready.clear();
    m.eof = false;
    m.need_key = m.video >= 0;
    return true;
}

}  // namespace wnk::media
