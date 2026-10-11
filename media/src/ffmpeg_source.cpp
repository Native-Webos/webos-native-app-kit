// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/ffmpeg_source.hpp"

#include "wnk/log.hpp"
#include "wnk/media/subtitles.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

#include <time.h>

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

SubtitleKind subtitle_kind(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_SUBRIP:
        case AV_CODEC_ID_SRT:
        case AV_CODEC_ID_ASS:
        case AV_CODEC_ID_SSA:
        case AV_CODEC_ID_WEBVTT:
        case AV_CODEC_ID_MOV_TEXT:
        case AV_CODEC_ID_TEXT: return SubtitleKind::Text;
        case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
        case AV_CODEC_ID_DVD_SUBTITLE:
        case AV_CODEC_ID_DVB_SUBTITLE: return SubtitleKind::Bitmap;
        default: return SubtitleKind::None;
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
    FfmpegSourceOptions options;
    AVFormatContext *fmt = nullptr;
    AVIOContext *avio = nullptr;  // custom I/O for network URIs
    std::mutex reader_mutex;      // guards `reader` against interrupt() from other threads
    std::unique_ptr<net::ByteReader> reader;  // network or caller-supplied bytes (custom I/O)
    int64_t origin_us = kNoTime;  // set_time_origin(); kNoTime: the container's start time
    AVBSFContext *bsf = nullptr;
    AVCodecContext *dec = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    MediaInfo info;
    int video = -1, audio = -1;
    bool decode_audio = false;
    // AudioPath::Ac3: decoded audio is converted by `swr` (made for the first frame and remade when
    // the frames change) into planar float at the encoder's layout and rate, collected in `fifo` and
    // encoded in frames of enc->frame_size samples. enc_base_us is the time of the fifo's first
    // sample counted from the last discontinuity, enc_samples the samples encoded since then.
    AVCodecContext *enc = nullptr;
    SwrContext *swr = nullptr;
    AVAudioFifo *fifo = nullptr;
    AVFrame *enc_frame = nullptr;
    int swr_format = -1, swr_rate = 0;
    AVChannelLayout swr_layout{};
    int64_t enc_base_us = kNoTime;
    int64_t enc_samples = 0;
    int64_t next_in_us = kNoTime;  // where the next decoded frame should start if there is no gap
    std::vector<uint8_t *> enc_planes;
    std::vector<float> enc_buffer;
    double enc_cpu_s = 0;          // thread CPU time spent converting and encoding, for the log
    int64_t enc_cpu_samples = 0;   // output samples that took
    std::vector<uint8_t> adts_config;  // AAC passthrough to a backend that wants ADTS: the AudioSpecificConfig
    int64_t start_us = 0;  // subtracted from every timestamp
    // Passthrough audio: where the next frame starts. MPEG-TS carries several AAC frames per PES and
    // only the first has a PTS; a stream opened mid-way (adaptive streaming) leaves the rest unset.
    int64_t next_audio_us = kNoTime;
    bool need_key = false;  // after a seek: drop video until a keyframe
    bool eof = false;
    std::deque<Packet> ready;
    std::deque<AVPacket *> replay;  // demuxed by seek() while looking for a keyframe; read() goes first
    std::atomic<bool> interrupted{false};
    std::vector<float> planar_tmp;
    std::map<int, AVCodecContext *> subs;  // stream index → decoder, for every decodable subtitle track

    void free_subtitle_decoders() {
        for (auto &[index, ctx] : subs) avcodec_free_context(&ctx);
        subs.clear();
    }

    ~Impl() { close(); }

    void free_ac3() {
        avcodec_free_context(&enc);
        swr_free(&swr);
        if (fifo != nullptr) av_audio_fifo_free(fifo);
        fifo = nullptr;
        av_frame_free(&enc_frame);
        av_channel_layout_uninit(&swr_layout);
        swr_format = -1;
        reset_ac3();
    }

    // Forgets the audio waiting to be encoded (seek, discontinuity).
    void reset_ac3() {
        if (fifo != nullptr) av_audio_fifo_reset(fifo);
        swr_free(&swr);  // remade with an empty history by the next frame
        enc_base_us = kNoTime;
        enc_samples = 0;
        next_in_us = kNoTime;
    }

    // Opens the AC-3 encoder for a track decoded by `dec`.
    bool open_ac3(const TrackInfo &t, std::string *error) {
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AC3);
        enc = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
        if (enc == nullptr) {
            *error = "no AC-3 encoder";
            return false;
        }
        const int ch = t.channels > 0 ? t.channels : dec->ch_layout.nb_channels;
        if (ch == 1) {
            enc->ch_layout = AV_CHANNEL_LAYOUT_MONO;
        } else if (ch == 2) {
            enc->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        } else {
            enc->ch_layout = AV_CHANNEL_LAYOUT_5POINT1;
        }
        const int rate = t.sample_rate > 0 ? t.sample_rate : dec->sample_rate;
        enc->sample_rate = rate == 48000 || rate == 44100 || rate == 32000 ? rate : 48000;
        enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        // The usual rates for discs and broadcast: transparent enough for a re-encode.
        enc->bit_rate = ch == 1 ? 192000 : ch == 2 ? 256000 : 640000;
        enc->time_base = AVRational{1, enc->sample_rate};
        if (avcodec_open2(enc, codec, nullptr) < 0) {
            *error = "could not open the AC-3 encoder";
            avcodec_free_context(&enc);
            return false;
        }
        const int out_ch = enc->ch_layout.nb_channels;
        fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, out_ch, enc->frame_size * 4);
        enc_frame = av_frame_alloc();
        if (fifo == nullptr || enc_frame == nullptr) {
            *error = "out of memory";
            free_ac3();
            return false;
        }
        return true;
    }

    // (Re)creates the converter when a decoded frame's format differs from the last one's.
    bool ensure_swr(const AVFrame *f) {
        AVChannelLayout in{};
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_default(&in, f->ch_layout.nb_channels);
        } else {
            av_channel_layout_copy(&in, &f->ch_layout);
        }
        if (swr != nullptr && swr_format == f->format && swr_rate == f->sample_rate &&
            av_channel_layout_compare(&swr_layout, &in) == 0) {
            av_channel_layout_uninit(&in);
            return true;
        }
        swr_free(&swr);
        av_channel_layout_uninit(&swr_layout);
        swr_layout = in;
        swr_format = f->format;
        swr_rate = f->sample_rate;
        if (swr_alloc_set_opts2(&swr, &enc->ch_layout, AV_SAMPLE_FMT_FLTP, enc->sample_rate, &swr_layout,
                                static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr) < 0 ||
            swr_init(swr) < 0) {
            WNK_LOGW(kCat, "cannot convert %d-channel %s at %d Hz for AC-3", f->ch_layout.nb_channels,
                     av_get_sample_fmt_name(static_cast<AVSampleFormat>(f->format)), f->sample_rate);
            swr_free(&swr);
            return false;
        }
        return true;
    }

    // Converts one decoded frame (nullptr: flush the converter) into the fifo.
    void convert_ac3(const AVFrame *f) {
        if (swr == nullptr) return;
        const int in_samples = f != nullptr ? f->nb_samples : 0;
        const int max = swr_get_out_samples(swr, in_samples);
        if (max <= 0) return;
        const int out_ch = enc->ch_layout.nb_channels;
        enc_buffer.resize(static_cast<size_t>(out_ch) * max);
        enc_planes.resize(static_cast<size_t>(out_ch));
        for (int c = 0; c < out_ch; ++c) {
            enc_planes[c] = reinterpret_cast<uint8_t *>(enc_buffer.data() + static_cast<size_t>(c) * max);
        }
        const int got = swr_convert(swr, enc_planes.data(), max,
                                    f != nullptr ? const_cast<const uint8_t **>(f->extended_data) : nullptr, in_samples);
        if (got > 0) av_audio_fifo_write(fifo, reinterpret_cast<void **>(enc_planes.data()), got);
    }

    // Encodes every full frame in the fifo; at the end (`last`) also the rest, padded with silence.
    void encode_ac3(bool last) {
        const int fs = enc->frame_size;
        while (av_audio_fifo_size(fifo) >= fs || (last && av_audio_fifo_size(fifo) > 0)) {
            av_frame_unref(enc_frame);
            enc_frame->nb_samples = fs;
            enc_frame->format = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_copy(&enc_frame->ch_layout, &enc->ch_layout);
            enc_frame->sample_rate = enc->sample_rate;
            if (av_frame_get_buffer(enc_frame, 0) < 0) return;
            const int n = av_audio_fifo_read(fifo, reinterpret_cast<void **>(enc_frame->extended_data), fs);
            if (n < fs) {
                av_samples_set_silence(enc_frame->extended_data, n, fs - n, enc->ch_layout.nb_channels,
                                       AV_SAMPLE_FMT_FLTP);
            }
            const int64_t pts_us = enc_base_us + enc_samples * 1000000 / enc->sample_rate;
            enc_frame->pts = enc_samples;
            enc_samples += fs;
            // The AC-3 encoder has no delay: one frame in, one packet out.
            if (avcodec_send_frame(enc, enc_frame) < 0) continue;
            AVPacket *out = av_packet_alloc();
            while (out != nullptr && avcodec_receive_packet(enc, out) == 0) {
                Packet a;
                a.type = TrackType::Audio;
                a.key = true;
                a.data.assign(out->data, out->data + out->size);
                a.pts_us = pts_us;
                a.duration_us = static_cast<int64_t>(fs) * 1000000 / enc->sample_rate;
                ready.push_back(std::move(a));
                av_packet_unref(out);
            }
            av_packet_free(&out);
        }
    }

    // Takes one decoded frame into the AC-3 path.
    void queue_ac3(const AVFrame *f, AVRational tb) {
        if (f->nb_samples <= 0 || f->sample_rate <= 0) return;
        const int64_t pts = to_us(f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts, tb);
        // A gap or overlap of more than 100 ms (a cut in a TS, a damaged frame) restarts the timeline
        // there instead of drifting off the video.
        if (pts != kNoTime && next_in_us != kNoTime && std::llabs(pts - next_in_us) > 100000) {
            WNK_LOGW(kCat, "audio jumps by %lld ms; AC-3 timeline restarted", static_cast<long long>((pts - next_in_us) / 1000));
            reset_ac3();
        }
        if (!ensure_swr(f)) return;
        if (enc_base_us == kNoTime) {
            if (pts == kNoTime) return;  // wait for a timed frame
            enc_base_us = pts;
            enc_samples = 0;
        }
        const int64_t start = pts != kNoTime ? pts : next_in_us;
        next_in_us = start + static_cast<int64_t>(f->nb_samples) * 1000000 / f->sample_rate;
        const double t0 = thread_cpu_s();
        const int64_t before = enc_samples;
        convert_ac3(f);
        encode_ac3(false);
        enc_cpu_s += thread_cpu_s() - t0;
        enc_cpu_samples += enc_samples - before;
        // Every 10 s of audio: what the re-encode costs (decoding not included).
        if (enc_cpu_samples >= static_cast<int64_t>(enc->sample_rate) * 10) {
            const double audio_s = static_cast<double>(enc_cpu_samples) / enc->sample_rate;
            WNK_LOGI(kCat, "AC-3 re-encode: %.1f ms CPU per s of audio (%.1f%% of a core)", enc_cpu_s * 1000 / audio_s,
                     enc_cpu_s * 100 / audio_s);
            enc_cpu_s = 0;
            enc_cpu_samples = 0;
        }
    }

    static double thread_cpu_s() {
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
    }

    void clear_replay() {
        for (AVPacket *p : replay) av_packet_free(&p);
        replay.clear();
    }

    void close() {
        clear_replay();
        free_subtitle_decoders();
        av_bsf_free(&bsf);
        free_ac3();
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);  // leaves a custom AVIOContext alone
        if (avio != nullptr) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
        {
            std::unique_ptr<net::ByteReader> gone;
            {
                std::lock_guard<std::mutex> lock(reader_mutex);
                gone = std::move(reader);
            }
        }
        av_packet_free(&pkt);
        av_frame_free(&frame);
        ready.clear();
        info = MediaInfo{};
        video = audio = -1;
        decode_audio = false;
        eof = false;
    }

    static int interrupt_cb(void *opaque) { return static_cast<Impl *>(opaque)->interrupted.load() ? 1 : 0; }

    // AVIOContext callbacks over the HTTP stream (playback thread only).
    static int avio_read(void *opaque, uint8_t *buf, int size) {
        auto *s = static_cast<net::ByteReader *>(opaque);
        const int64_t n = s->read(buf, static_cast<size_t>(size));
        if (n > 0) return static_cast<int>(n);
        if (n == 0) return AVERROR_EOF;
        return s->error() == net::NetError::Cancelled ? AVERROR_EXIT : AVERROR(EIO);
    }
    static int64_t avio_seek(void *opaque, int64_t offset, int whence) {
        auto *s = static_cast<net::ByteReader *>(opaque);
        switch (whence & ~AVSEEK_FORCE) {
            case AVSEEK_SIZE: return s->size() >= 0 ? s->size() : AVERROR(ENOSYS);
            case SEEK_SET: break;
            case SEEK_CUR: offset += s->position(); break;
            case SEEK_END:
                if (s->size() < 0) return AVERROR(ENOSYS);
                offset += s->size();
                break;
            default: return AVERROR(EINVAL);
        }
        return s->seek(offset) ? offset : AVERROR(EIO);
    }

    // Connects to an http(s) URI and gives `fmt` a custom AVIOContext reading from it.
    bool open_http(const std::string &uri, std::string *error) {
        if (!options.http) {
            *error = "no HTTP transport for network URIs";
            return false;
        }
        auto stream = std::make_unique<net::HttpStream>(options.http, uri, options.http_stream);
        net::HttpStream *s = stream.get();
        adopt(std::move(stream));
        if (!s->open()) {
            const long st = s->http_status();
            *error = s->error() == net::NetError::Cancelled ? "interrupted"
                     : st != 0                              ? "HTTP " + std::to_string(st)
                                                            : s->error_detail();
            return false;
        }
        return attach(s, error);
    }

    // Takes ownership of the reader (interruptible from now on).
    void adopt(std::unique_ptr<net::ByteReader> r) {
        std::lock_guard<std::mutex> lock(reader_mutex);
        reader = std::move(r);
        if (interrupted) reader->interrupt();  // interrupt() came before the reader existed
    }

    // Gives `fmt` a custom AVIOContext reading from `s`.
    bool attach(net::ByteReader *s, std::string *error) {
        constexpr int kBufferSize = 64 * 1024;
        auto *buffer = static_cast<unsigned char *>(av_malloc(kBufferSize));
        avio = buffer != nullptr ? avio_alloc_context(buffer, kBufferSize, 0, s, &avio_read, nullptr,
                                                      s->seekable() ? &avio_seek : nullptr)
                                 : nullptr;
        if (avio == nullptr) {
            av_free(buffer);
            *error = "out of memory";
            return false;
        }
        // Without byte ranges only SEEK_CUR forward within the buffer works, so FFmpeg must not
        // count on seeking (an MP4 with its index at the end cannot be played that way).
        avio->seekable = s->seekable() ? AVIO_SEEKABLE_NORMAL : 0;
        fmt->pb = avio;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        return true;
    }

    // Prefers the HTTP stream's reason over FFmpeg's generic I/O error.
    std::string io_error(int rc) {
        std::lock_guard<std::mutex> lock(reader_mutex);
        if (reader && rc == AVERROR(EIO) && reader->error() != net::NetError::None) {
            const long st = reader->http_status();
            return st != 0 ? "HTTP " + std::to_string(st) : reader->error_detail();
        }
        return av_err(rc);
    }

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

    // Decodes one subtitle packet into a cue (text, bitmaps, or a clear event).
    void decode_subtitle(AVCodecContext *ctx, AVPacket *p, AVRational tb) {
        AVSubtitle sub{};
        int got = 0;
        if (avcodec_decode_subtitle2(ctx, &sub, &got, p) < 0 || got == 0) return;
        auto cue = std::make_shared<SubtitleCue>();
        cue->track = p->stream_index;
        const int64_t pts = to_us(p->pts != AV_NOPTS_VALUE ? p->pts : p->dts, tb);
        if (pts == kNoTime) {
            avsubtitle_free(&sub);
            return;
        }
        cue->start_us = pts + static_cast<int64_t>(sub.start_display_time) * 1000;
        if (p->duration > 0) {
            cue->end_us = pts + av_rescale_q(p->duration, tb, kMicros);
        } else if (sub.end_display_time != 0 && sub.end_display_time != UINT32_MAX) {
            cue->end_us = pts + static_cast<int64_t>(sub.end_display_time) * 1000;
        }
        cue->canvas_w = ctx->width;
        cue->canvas_h = ctx->height;
        for (unsigned r = 0; r < sub.num_rects; ++r) {
            const AVSubtitleRect *rect = sub.rects[r];
            std::string line;
            if (rect->type == SUBTITLE_ASS && rect->ass != nullptr) {
                line = ass_event_text(rect->ass);
            } else if (rect->type == SUBTITLE_TEXT && rect->text != nullptr) {
                line = rect->text;
            } else if (rect->type == SUBTITLE_BITMAP && rect->w > 0 && rect->h > 0 && rect->data[0] != nullptr &&
                       rect->data[1] != nullptr) {
                // PAL8: one index per pixel, a palette of 0xAARRGGBB words.
                SubtitleBitmap b;
                b.x = rect->x;
                b.y = rect->y;
                b.width = rect->w;
                b.height = rect->h;
                b.rgba.resize(static_cast<size_t>(rect->w) * rect->h * 4);
                const auto *palette = reinterpret_cast<const uint32_t *>(rect->data[1]);
                for (int y = 0; y < rect->h; ++y) {
                    const uint8_t *src = rect->data[0] + static_cast<ptrdiff_t>(y) * rect->linesize[0];
                    uint8_t *dst = b.rgba.data() + static_cast<size_t>(y) * rect->w * 4;
                    for (int x = 0; x < rect->w; ++x) {
                        const uint32_t c = palette[src[x]];
                        dst[x * 4 + 0] = static_cast<uint8_t>(c >> 16);
                        dst[x * 4 + 1] = static_cast<uint8_t>(c >> 8);
                        dst[x * 4 + 2] = static_cast<uint8_t>(c);
                        dst[x * 4 + 3] = static_cast<uint8_t>(c >> 24);
                    }
                }
                cue->bitmaps.push_back(std::move(b));
            }
            if (!line.empty()) cue->text += (cue->text.empty() ? "" : "\n") + line;
        }
        avsubtitle_free(&sub);
        if (!cue->bitmaps.empty() && (cue->canvas_w <= 0 || cue->canvas_h <= 0)) {
            // DVD subtitles without a size: assume the video's frame (or PAL SD).
            const TrackInfo *v = info.best(TrackType::Video);
            cue->canvas_w = v != nullptr && v->width > 0 ? v->width : 720;
            cue->canvas_h = v != nullptr && v->height > 0 ? v->height : 576;
        }
        Packet out;
        out.type = TrackType::Subtitle;
        out.key = true;
        out.pts_us = cue->start_us;
        out.subtitle = std::move(cue);
        ready.push_back(std::move(out));
    }

    // Turns one demuxed packet of a selected stream into ready packets.
    void consume(AVPacket *p) {
        const int si = p->stream_index;
        const AVRational tb = fmt->streams[si]->time_base;
        if (const auto sd = subs.find(si); sd != subs.end()) {
            decode_subtitle(sd->second, p, tb);
            return;
        }
        if (si == video) {
            if (bsf != nullptr) {
                if (av_bsf_send_packet(bsf, p) == 0) drain_bsf(tb);  // takes the packet's reference
            } else {
                queue_video(p, tb);
            }
        } else if (si == audio) {
            if (decode_audio) {
                if (avcodec_send_packet(dec, p) == 0) drain_decoder(tb);
            } else if (!need_key || video < 0) {
                Packet a;
                a.type = TrackType::Audio;
                a.key = true;
                uint8_t adts[7];
                const bool framed = p->size >= 2 && p->data[0] == 0xFF && (p->data[1] & 0xF0) == 0xF0;
                if (!adts_config.empty() && !framed && adts_header(adts_config, static_cast<size_t>(p->size), adts)) {
                    a.data.assign(adts, adts + 7);
                    a.data.insert(a.data.end(), p->data, p->data + p->size);
                } else {
                    a.data.assign(p->data, p->data + p->size);
                }
                a.pts_us = to_us(p->pts != AV_NOPTS_VALUE ? p->pts : p->dts, tb);
                a.duration_us = p->duration > 0 ? av_rescale_q(p->duration, tb, kMicros) : 0;
                const AVCodecParameters *par = fmt->streams[si]->codecpar;
                if (a.duration_us == 0 && par->frame_size > 0 && par->sample_rate > 0) {
                    a.duration_us = static_cast<int64_t>(par->frame_size) * 1000000 / par->sample_rate;
                }
                if (a.pts_us == kNoTime) a.pts_us = next_audio_us;
                if (a.pts_us == kNoTime) return;  // nothing timed yet: an untimed frame cannot be placed
                next_audio_us = a.duration_us > 0 ? a.pts_us + a.duration_us : kNoTime;
                ready.push_back(std::move(a));
            }
        }
    }

    enum class Scan { Found, NotFound, NoVideo };

    // After a seek: demuxes forward to the target `limit` (video stream time base) and keeps, for
    // read(), everything from the last video keyframe at or before it. NotFound if the video passes
    // the target (or the stream ends) without one, so the caller must start further back; replay then
    // holds the overshooting packet if it is a keyframe.
    Scan scan_to_keyframe(int64_t limit) {
        clear_replay();
        bool found = false, saw_video = false;
        for (int n = 0; n < 50000; ++n) {
            AVPacket *p = av_packet_alloc();
            if (p == nullptr || av_read_frame(fmt, p) < 0) {
                av_packet_free(&p);
                break;  // end, error or interrupt: read() will meet it again
            }
            const bool is_video = p->stream_index == video;
            const bool wanted = is_video || p->stream_index == audio;
            if (subs.count(p->stream_index) != 0) {
                consume(p);  // subtitles are kept whatever the landing point (the store drops repeats)
                av_packet_free(&p);
                continue;
            }
            if (is_video) {
                saw_video = true;
                const int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
                if (ts != AV_NOPTS_VALUE && ts > limit) {
                    if (!found) {
                        // Kept in case the caller cannot go further back (nothing precedes it).
                        if ((p->flags & AV_PKT_FLAG_KEY) != 0) {
                            replay.push_back(p);
                        } else {
                            av_packet_free(&p);
                        }
                        return Scan::NotFound;
                    }
                    replay.push_back(p);
                    return Scan::Found;
                }
                if ((p->flags & AV_PKT_FLAG_KEY) != 0) {
                    clear_replay();  // a later keyframe, still before the target
                    found = true;
                }
            }
            if (wanted && found) {
                replay.push_back(p);
            } else {
                av_packet_free(&p);
            }
        }
        return found ? Scan::Found : saw_video ? Scan::NotFound : Scan::NoVideo;
    }

    void drain_decoder(AVRational tb) {
        while (avcodec_receive_frame(dec, frame) == 0) {
            if (enc != nullptr) {
                queue_ac3(frame, tb);
            } else {
                queue_pcm(frame, tb);
            }
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
        v.dts_us = to_us(p->dts, tb);
        v.duration_us = p->duration > 0 ? av_rescale_q(p->duration, tb, kMicros) : 0;
        ready.push_back(std::move(v));
    }
};

FfmpegSource::FfmpegSource(FfmpegSourceOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
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
void FfmpegSource::interrupt() {
    impl_->interrupted = true;
    std::lock_guard<std::mutex> lock(impl_->reader_mutex);
    if (impl_->reader) impl_->reader->interrupt();
}
void FfmpegSource::reset_interrupt() {
    impl_->interrupted = false;
    std::lock_guard<std::mutex> lock(impl_->reader_mutex);
    if (impl_->reader) impl_->reader->reset_interrupt();
}

bool FfmpegSource::open(const std::string &uri, std::string *error) {
    Impl &m = *impl_;
    m.close();
    const bool network = uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0;
    std::string path = uri;
    if (path.rfind("file://", 0) == 0) path = path.substr(7);
    if (!network && (path.empty() || path[0] != '/')) {
        *error = "only local files (file:// or absolute paths) and http(s) URIs are supported";
        return false;
    }
    m.fmt = avformat_alloc_context();
    m.fmt->interrupt_callback = {&Impl::interrupt_cb, &m};
    if (network && !m.open_http(uri, error)) {
        m.close();
        return false;
    }
    // With custom I/O the name only hints the format (by extension): pass the file name alone, as a
    // query would hide the extension and an http(s) URL makes FFmpeg look for network protocols.
    std::string name = path;
    if (network) {
        name = uri.substr(0, uri.find_first_of("?#"));
        name = name.substr(name.find_last_of('/') + 1);
    }
    return open_input(name, uri, error);
}

bool FfmpegSource::open_reader(std::unique_ptr<net::ByteReader> reader, const std::string &name, std::string *error) {
    Impl &m = *impl_;
    m.close();
    m.fmt = avformat_alloc_context();
    m.fmt->interrupt_callback = {&Impl::interrupt_cb, &m};
    net::ByteReader *r = reader.get();
    m.adopt(std::move(reader));
    if (!m.attach(r, error)) {
        m.close();
        return false;
    }
    const std::string hint = name.substr(0, name.find_first_of("?#"));
    return open_input(hint.substr(hint.find_last_of('/') + 1), name, error);
}

void FfmpegSource::set_time_origin(int64_t origin_us) { impl_->origin_us = origin_us; }
int64_t FfmpegSource::time_origin() const { return impl_->start_us; }

bool FfmpegSource::open_input(const std::string &name, const std::string &uri, std::string *error) {
    Impl &m = *impl_;
    int rc = avformat_open_input(&m.fmt, name.c_str(), nullptr, nullptr);
    if (rc < 0) {
        // avformat_open_input frees the context on failure (but not a custom AVIOContext).
        *error = rc == AVERROR_EXIT ? "interrupted" : m.io_error(rc);
        m.close();
        return false;
    }
    rc = avformat_find_stream_info(m.fmt, nullptr);
    if (rc < 0) {
        *error = rc == AVERROR_EXIT ? "interrupted" : "stream info: " + m.io_error(rc);
        m.close();
        return false;
    }
    m.pkt = av_packet_alloc();
    m.frame = av_frame_alloc();
    m.start_us = m.origin_us != kNoTime ? m.origin_us : m.fmt->start_time != AV_NOPTS_VALUE ? m.fmt->start_time : 0;
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
                t.decodable = avcodec_find_decoder(par->codec_id) != nullptr;
                t.ac3_encodable = t.decodable && m.options.ac3_encode && avcodec_find_encoder(AV_CODEC_ID_AC3) != nullptr;
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                t.type = TrackType::Subtitle;
                // Only what this FFmpeg build can decode is offered.
                t.subtitle = avcodec_find_decoder(par->codec_id) != nullptr ? subtitle_kind(par->codec_id)
                                                                             : SubtitleKind::None;
                break;
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
    m.clear_replay();
    m.free_subtitle_decoders();
    av_bsf_free(&m.bsf);
    m.free_ac3();
    avcodec_free_context(&m.dec);
    m.video = m.audio = -1;
    for (const TrackInfo &t : m.info.tracks) {
        if (t.type != TrackType::Subtitle || t.subtitle == SubtitleKind::None) continue;
        const AVStream *st = m.fmt->streams[t.index];
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        AVCodecContext *ctx = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
        if (ctx != nullptr && avcodec_parameters_to_context(ctx, st->codecpar) >= 0) {
            ctx->pkt_timebase = st->time_base;
            if (avcodec_open2(ctx, codec, nullptr) >= 0) {
                m.subs[t.index] = ctx;
                continue;
            }
        }
        WNK_LOGW(kCat, "subtitle track %d (%s) cannot be decoded", t.index, t.codec_label.c_str());
        avcodec_free_context(&ctx);
    }
    m.decode_audio = false;
    m.next_audio_us = kNoTime;
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
        // Dolby Vision configuration record (MP4 dvcC/dvvC, Matroska block addition mapping).
        if (const AVPacketSideData *sd = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                                                 AV_PKT_DATA_DOVI_CONF)) {
            const auto *dv = reinterpret_cast<const AVDOVIDecoderConfigurationRecord *>(sd->data);
            if (t->codec == CodecId::Hevc && dv->rpu_present_flag != 0 && dv->el_present_flag == 0) {
                out->video->dolby_vision_profile = dv->dv_profile;
            }
            WNK_LOGI(kCat, "Dolby Vision profile %d, compatibility %d%s", dv->dv_profile, dv->dv_bl_signal_compatibility_id,
                     dv->el_present_flag != 0 ? ", enhancement layer (base layer only)" : "");
        }
    }

    if (audio_track >= 0) {
        const TrackInfo *t = m.info.track(audio_track);
        if (t == nullptr || t->type != TrackType::Audio) {
            *error = "no audio track " + std::to_string(audio_track);
            return false;
        }
        const AVCodecParameters *par = m.fmt->streams[audio_track]->codecpar;
        const AudioPath path = audio_path(caps, *t);
        if (path == AudioPath::Passthrough) {
            AudioFormat a{t->codec, t->sample_rate, t->channels, {}};
            m.adts_config.clear();
            if (t->codec == CodecId::Aac && caps.aac_adts && par->extradata_size >= 2) {
                m.adts_config.assign(par->extradata, par->extradata + par->extradata_size);
            }
            if (par->extradata_size > 0) a.codec_header.assign(par->extradata, par->extradata + par->extradata_size);
            a.atmos = t->codec == CodecId::Eac3 && par->profile == AV_PROFILE_EAC3_DDP_ATMOS;
            out->audio = a;
            m.audio = audio_track;
        } else if (path == AudioPath::Pcm || path == AudioPath::Ac3) {
            const AVCodec *codec = avcodec_find_decoder(par->codec_id);
            m.dec = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
            if (m.dec == nullptr || avcodec_parameters_to_context(m.dec, par) < 0) {
                *error = "no decoder for " + t->codec_label;
                avcodec_free_context(&m.dec);
                return false;
            }
            m.dec->pkt_timebase = m.fmt->streams[audio_track]->time_base;
            // AC-3/E-AC-3 can downmix inside the decoder, which also applies the stream's mix levels.
            if (path == AudioPath::Pcm && (t->codec == CodecId::Ac3 || t->codec == CodecId::Eac3)) {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                av_opt_set_chlayout(m.dec->priv_data, "downmix", &stereo, 0);
            }
            if (avcodec_open2(m.dec, codec, nullptr) < 0) {
                *error = "could not open the " + t->codec_label + " decoder";
                avcodec_free_context(&m.dec);
                return false;
            }
            if (path == AudioPath::Ac3) {
                if (!m.open_ac3(*t, error)) {
                    avcodec_free_context(&m.dec);
                    return false;
                }
                out->audio = AudioFormat{CodecId::Ac3, m.enc->sample_rate, m.enc->ch_layout.nb_channels, {}};
                WNK_LOGI(kCat, "audio %s %d ch %d Hz re-encoded to AC-3 %d ch %d Hz %lld kb/s", t->codec_label.c_str(),
                         t->channels, t->sample_rate, out->audio->channels, out->audio->sample_rate,
                         static_cast<long long>(m.enc->bit_rate / 1000));
            } else {
                out->audio = AudioFormat{CodecId::PcmS16, t->sample_rate, 2, {}};
            }
            m.decode_audio = true;
            m.audio = audio_track;
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
        if (!m.replay.empty()) {
            AVPacket *p = m.replay.front();
            m.replay.pop_front();
            m.consume(p);
            av_packet_free(&p);
            continue;
        }
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
                if (m.enc != nullptr && m.enc_base_us != kNoTime) {
                    m.convert_ac3(nullptr);
                    m.encode_ac3(true);
                }
            }
            continue;
        }
        if (rc < 0) {
            if (rc == AVERROR_EXIT) return Read::Interrupted;
            *error = m.io_error(rc);
            return Read::Error;
        }
        m.consume(m.pkt);
        av_packet_unref(m.pkt);
    }
}

bool FfmpegSource::seek(int64_t pts_us, std::string *error) {
    Impl &m = *impl_;
    if (m.fmt == nullptr) {
        *error = "not open";
        return false;
    }
    // Seek on the video stream when there is one, so the landing point is a video keyframe. Formats
    // with a keyframe index (MP4, MKV) land on one; MPEG-TS and other index-less formats land on the
    // timestamp (before or after it), so read forward to the target and replay from the last keyframe
    // before it, starting further back while there is none.
    const int stream = m.video >= 0 ? m.video : -1;
    m.clear_replay();
    m.ready.clear();
    for (auto &[index, ctx] : m.subs) avcodec_flush_buffers(ctx);
    int64_t back_us = 0;
    for (;;) {
        const int64_t from_us = std::max<int64_t>(pts_us - back_us, 0);
        const int64_t ts = stream >= 0 ? av_rescale_q(from_us + m.start_us, kMicros, m.fmt->streams[stream]->time_base)
                                       : from_us + m.start_us;
        int rc = -1;
        // A timestamp search for the start can land after it; go to the first byte instead.
        if (from_us == 0 && back_us > 0) rc = av_seek_frame(m.fmt, -1, 0, AVSEEK_FLAG_BYTE);
        if (rc < 0) rc = av_seek_frame(m.fmt, stream, ts, AVSEEK_FLAG_BACKWARD);
        if (rc < 0) {
            m.clear_replay();
            *error = m.io_error(rc);
            return false;
        }
        if (stream < 0) break;
        const int64_t limit = av_rescale_q(pts_us + m.start_us, kMicros, m.fmt->streams[stream]->time_base);
        if (m.scan_to_keyframe(limit) != Impl::Scan::NotFound || m.interrupted || (from_us == 0 && back_us > 0) ||
            back_us >= 16000000) {
            break;
        }
        back_us = back_us == 0 ? 1000000 : back_us * 2;  // 1, 2, 4, 8, 16 s
    }
    if (m.bsf != nullptr) av_bsf_flush(m.bsf);
    if (m.dec != nullptr) avcodec_flush_buffers(m.dec);
    m.reset_ac3();
    m.next_audio_us = kNoTime;
    // Keep the subtitles decoded while looking for the keyframe; drop everything else.
    m.ready.erase(std::remove_if(m.ready.begin(), m.ready.end(),
                                 [](const Packet &p) { return p.type != TrackType::Subtitle; }),
                  m.ready.end());
    m.eof = false;
    m.need_key = m.video >= 0;
    return true;
}

}  // namespace wnk::media
