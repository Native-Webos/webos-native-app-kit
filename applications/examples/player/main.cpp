// SPDX-License-Identifier: GPL-3.0-or-later
//
// Example D: native video playback with the kit's media foundation.
//
//   clip list ──OK──► player screen: FfmpegSource → PlaybackController → NdlBackend (TV)
//                                                                      → PacingBackend (host: no output)
//
// The bundled clips (tools/make-test-clips.sh) flash white and beep during the first 80 ms of every
// second, so on the TV picture/sound sync and the NDL PTS unit can be judged by eye and ear.
//
// Player keys: OK or Play/Pause toggles pause, Left/Right seek 10 s, Up opens the audio, subtitle and
// playback speed menu, Down cycles the subtitle tracks (a track the file marks as default starts on), Stop or
// Back leaves. Changing the audio track reloads the decoder at the position (a short gap). Any key
// shows the controls; they hide after 4 s of playback. Pause and seek keep the decoder loaded, so the
// picture holds; backgrounding the app pauses playback and releases the decoder, and playback resumes
// on return if it was playing.
//
// Launch params (ares-launch --params):
//   {"uri": "file:///path/to/file.mkv"}   play that file straight away; http(s):// URIs stream through
//                                          the kit's HTTP client (byte ranges make them seekable); .mpd
//                                          and .m3u8 URIs play as DASH/HLS (AdaptiveSource)
//   {"lang": "de"}                         preferred audio language for DASH/HLS
//   {"keys": "kid:key,kid:key"}            ClearKey content keys (32 hex digits each) for CENC MP4/DASH
//   {"licenseUrl": "https://..."}          ClearKey licence server (else the one the manifest names)
//   {"seeks": "15:+10,17:+10"}             scripted relative seeks (seconds after the player opens:
//                                          seconds to seek), for reproducing seek bugs without a remote
//   {"startKbps": 1000}                    DASH/HLS: throughput assumed before the first measurement
//                                          (default 6000), to start low and watch the bitrate climb
//   {"capKbps": "0:800,20:0"}              scripted video bandwidth caps (seconds after the player opens:
//                                          kb/s, 0 lifts the cap), to force bitrate switches on demand
//   {"codecs": "h264"}                     DASH/HLS: only renditions of this video codec (h264, hevc)
//   {"audio": "5:2,9:1"}                   scripted audio track changes (seconds after the player opens:
//                                          stream index), for checking the switch without a remote
//   {"speeds": "5:1.5,15:1"}               scripted playback speeds (seconds after the player opens: speed)
//   {"thumbs": "http://.../thumbs.vtt"}   seek previews for a plain file (WebVTT cues naming
//                                          "sheet.jpg#xywh=x,y,w,h"); DASH/HLS bring their own
//   {"subs": ["http://.../film.srt"]}      subtitle files beside the media (SubRip, WebVTT, ASS, TTML; a
//                                          string or a list), added as tracks; the first is shown
//   {"ptsUnit": "ms"}                      feed NDL millisecond PTS instead of microseconds (experiment)
//   {"backend": "ndl"}                     force NDL (default: Starfish when the codecs suit it, else NDL)
//   {"backend": "starfish"}                force LG's Starfish pipeline
//   {"seekMode": "splice"}                 Starfish seeks by splicing the timeline instead of LG's Seek
//   {"pcmLeadMs": 250}                     how much later Starfish's decoded PCM is stamped than the
//                                          picture (default: by the TV's audio stack and sound output)
//   {"ndlReload": 1}                       pause and seek by reloading NDL (black picture; comparison)
//   {"leadMs": 400}                        feed at most this far ahead of the clock (default 1000)
//   {"ac3": false}                         never re-encode audio to AC-3 (audio Starfish cannot take
//                                          then sends the file to NDL, as before)
//   {"drmTest": {"drm": "playready", "uri": ..., "la": ..., "transport": "MPEG-DASH", "type": "media"}}
//                                          ("drm": "none" loads the URI the same way without DRM;
//                                          "transport": "" leaves mediaTransportType out)
//                                          experiment: protected playback through LG's DRM service and
//                                          the platform pipeline (docs/research/DRM-FEASIBILITY.md)
//   {"autoplay": 0}                        open that bundled clip at start (0..5), or stream (6..16)
#include "wnk/app/app.hpp"
#include "wnk/log.hpp"
#include "wnk/storage/secret.hpp"
#include "wnk/media/auto_backend.hpp"
#include "wnk/media/controller.hpp"
#include "wnk/media/device_caps.hpp"
#include "wnk/media/adaptive_source.hpp"
#include "wnk/media/ffmpeg_source.hpp"
#include "wnk/media/ndl_backend.hpp"
#include "wnk/media/pacing_backend.hpp"
#include "wnk/media/starfish_backend.hpp"
#include "wnk/media/subtitle_view.hpp"
#include "wnk/media/thumbnails.hpp"
#include "wnk/platform/luna.hpp"
#include "wnk/platform/process.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/widgets.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <thread>
#include <optional>
#include <mutex>
#include <functional>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <dlfcn.h>

namespace {

using namespace wnk::ui;
using namespace wnk::media;
using wnk::input::Action;
using wnk::input::Phase;

constexpr const char *kCat = "player";

struct Clip {
    std::string title, detail, file;
};

// Public test streams (network), listed under the bundled clips.
struct Stream {
    Stream(std::string t, std::string d, std::string u, std::string k = {})
        : title(std::move(t)), detail(std::move(d)), url(std::move(u)), keys(std::move(k)) {}
    std::string title, detail, url;
    std::string keys;  // ClearKey "kid:key" pairs for a protected stream (public test keys only)
};

struct Settings {
    std::string clips_dir;
    PtsUnit pts_unit = PtsUnit::Microseconds;
    bool ndl_reload = false;
    std::string backend = "auto";  // auto (Starfish when the codecs suit it, else NDL), starfish, ndl
    bool native_seek = true;       // {"seekMode":"splice"}: Starfish splices instead of LG's Seek
    int pcm_lead_ms = -1;          // {"pcmLeadMs":250}: Starfish's PCM lead (default: by stack and output)
    int lead_ms = 1000;
    bool ac3 = true;  // re-encode audio Starfish cannot take to AC-3
    std::string language;  // preferred audio language for DASH/HLS
    std::vector<std::pair<double, double>> seeks;  // scripted: (seconds after opening, relative seek)
    std::vector<std::pair<double, double>> audio_switches;  // scripted: (seconds after opening, audio track)
    std::vector<std::pair<double, double>> speeds;          // scripted: (seconds after opening, playback speed)
    std::vector<std::pair<double, double>> caps;  // scripted: (seconds after opening, video cap in kb/s)
    int64_t start_bps = 0;                        // AdaptiveSourceOptions::initial_bandwidth; 0: default
    std::string codecs;                           // "h264" or "hevc": only that video codec; empty: by the TV
    DeviceMediaCaps device;
    std::shared_ptr<wnk::net::HttpTransport> http;  // null if libcurl is unavailable
    ContentKeys keys;         // ClearKey content keys ({"keys": ...})
    std::string license_url;  // ClearKey licence server ({"licenseUrl": ...}); else the manifest's
    std::vector<std::string> subtitle_files;  // {"subs": ...}: added as tracks beside the media
    std::string thumbs;  // {"thumbs": ".../thumbs.vtt"}: seek previews for a plain file
};

constexpr int kPreviewWidth = 320;  // seek preview tiles: the closest width the stream offers, not wider

std::string clock_text(int64_t us) {
    if (us == kNoTime || us < 0) return "--:--";
    const int64_t s = us / 1000000;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%lld:%02lld", static_cast<long long>(s / 60), static_cast<long long>(s % 60));
    return buf;
}

std::unique_ptr<MediaBackend> make_backend(const Settings &s) {
    if (!wnk::platform::kIsWebOS) return std::make_unique<PacingBackend>();
    std::string why_sf, why_ndl;
    const bool starfish = s.backend != "ndl" && StarfishBackend::available(&why_sf);
    const bool ndl = s.backend != "starfish" && NdlBackend::available(&why_ndl);
    auto make_ndl = [&s] {
        NdlOptions o;
        o.pts_unit = s.pts_unit;
        o.reload_to_pause_and_seek = s.ndl_reload;
        return std::make_unique<NdlBackend>(o);
    };
    // Dolby Vision and Atmos are declared only when the TV says yes (unknown counts as no).
    StarfishOptions so;
    so.dolby_vision = s.device.dolby_vision == true;
    so.atmos = s.device.dolby_atmos == true;
    so.native_seek = s.native_seek;
    so.pcm_lead_ms = s.pcm_lead_ms;
    if (starfish && ndl) return std::make_unique<AutoBackend>(std::make_unique<StarfishBackend>(so), make_ndl());
    if (starfish) return std::make_unique<StarfishBackend>(so);
    if (ndl) return make_ndl();
    WNK_LOGW(kCat, "no media backend (Starfish: %s; NDL: %s): no picture", why_sf.empty() ? "not wanted" : why_sf.c_str(),
             why_ndl.empty() ? "not wanted" : why_ndl.c_str());
    return std::make_unique<PacingBackend>();
}

// DASH/HLS manifests go to the AdaptiveSource, everything else to FfmpegSource. Renditions are
// chosen from what the TV reports: HEVC first when it has HEVC, 2160p only on a UHD panel.
// `adaptive` (if given) is set to the AdaptiveSource made, for the scripted bandwidth caps.
std::unique_ptr<MediaSource> make_source(const Settings &s, const std::string &uri, AdaptiveSource **adaptive = nullptr) {
    if (AdaptiveSource::handles(uri)) {
        AdaptiveSourceOptions o;
        o.http = s.http;
        o.ac3_encode = s.ac3;
        o.language = s.language;
        if (s.device.hevc == false) o.video_codecs = {CodecId::H264};
        if (s.device.uhd_panel != true) o.max_height = 1080;
        o.dolby_vision = s.device.dolby_vision == true;
        o.keys = s.keys;
        o.license_url = s.license_url;
        if (s.start_bps > 0) o.initial_bandwidth = s.start_bps;
        if (s.codecs == "h264") o.video_codecs = {CodecId::H264};
        if (s.codecs == "hevc") o.video_codecs = {CodecId::Hevc};
        auto src = std::make_unique<AdaptiveSource>(o);
        if (adaptive) *adaptive = src.get();
        return src;
    }
    FfmpegSourceOptions o;
    o.http = s.http;
    o.ac3_encode = s.ac3;
    o.decryption_keys = s.keys;
    return std::make_unique<FfmpegSource>(std::move(o));
}

// ---- player screen ---------------------------------------------------------------------------------

class PlayerScreen : public Screen {
 public:
    PlayerScreen(wnk::app::Context &ctx, const Settings &s, std::string uri, std::string title)
        : Screen(title), ctx_(ctx), uri_(std::move(uri)), seeks_(s.seeks), audio_switches_(s.audio_switches), speeds_(s.speeds), caps_(s.caps),
          subtitle_files_(s.subtitle_files), http_(s.http) {
        name = "player";
        thumbs_url_ = s.thumbs;
        // Under the controls: subtitles over the whole picture.
        subs_view_ = &add<SubtitleView>();
        subs_view_->set_width(Dim::fill()).set_height(Dim::fill());
        subs_view_->set_provider([this] { return controller_ ? controller_->subtitles_now() : SubtitleView::Cues{}; });
        subs_view_->set_clock([this] { return controller_ ? controller_->snapshot().position_us : kNoTime; });
        bar_ = &add<Column>(14);
        bar_->set_align(Align::Stretch, Align::End);
        bar_->set_padding({80, 40, 80, 60});
        bar_->set_background(wnk::render::Color::hex(0x000000B0));
        auto &top = bar_->add<Row>(24);
        title_ = &top.add<Label>(title, TextRole::Title);
        title_->set_width(Dim::fill());
        status_ = &top.add<Label>("Opening…");
        status_->set_muted(true).set_align(Align::Start, Align::End);
        // A seek preview over the progress bar, at the target's place along it (while seeking).
        preview_row_ = &bar_->add<Row>(0);
        auto &preview = preview_row_->add<Column>(6);
        preview.set_background(wnk::render::Color::hex(0x000000E0), 8);
        preview.set_padding(Insets::all(6));
        preview_ = &preview.add<ImageView>();
        preview_->set_fit(ImageFit::Contain);
        preview_->set_size(kPreviewWidth, kPreviewWidth * 9 / 16);
        preview_time_ = &preview.add<Label>("", TextRole::Small);
        preview_time_->set_text_align(Align::Center);
        preview_row_->set_visible(false);
        progress_ = &bar_->add<ProgressBar>();
        auto &bottom = bar_->add<Row>(24);
        time_ = &bottom.add<Label>("--:-- / --:--");
        time_->set_width(Dim::fill());
        audio_label_ = &bottom.add<Label>("", TextRole::Small);
        subs_label_ = &bottom.add<Label>("", TextRole::Small);
        bottom.add<Label>("OK pause  ·  Left/Right 10 s  ·  Up tracks  ·  Back leaves", TextRole::Small).set_muted(true);
        detail_ = &bar_->add<Label>("", TextRole::Small);
        detail_->set_muted(true);
        spinner_ = &add<Spinner>(96);

        auto backend = make_backend(s);
        backend_name_ = backend->name();
        PlaybackOptions po;
        po.max_lead_us = static_cast<int64_t>(s.lead_ms) * 1000;
        controller_ = std::make_unique<PlaybackController>(make_source(s, uri_, &adaptive_), std::move(backend), ctx.dispatcher(), po);
        controller_->set_state_callback([this](PlayState from, PlayState to, const PlaybackSnapshot &snap) {
            WNK_LOGI(kCat, "player state %s -> %s%s%s", play_state_name(from), play_state_name(to),
                     snap.error.empty() ? "" : ": ", snap.error.c_str());
            if (to == PlayState::Ready) {
                controller_->set_video_area({0, 0, 1920, 1080});  // again, now the stream size is known
                load_thumbnails();
                describe(snap);
                tracks_ = snap.info.tracks;
                for (const TrackInfo &t : tracks_) {
                    if (t.type == TrackType::Subtitle && t.subtitle != SubtitleKind::None && t.is_default) {
                        select_subtitles(t.index);
                        break;
                    }
                }
                if (controller_->subtitle_track() < 0) select_subtitles(-1);
            }
            show_controls();
        });
    }

    void on_show(Ui &ui) override {
        ui.set_clear_color(wnk::render::kTransparent);  // the video plane is under the UI
        controller_->set_video_area({0, 0, 1920, 1080});
        if (!opened_) {
            opened_ = true;
            WNK_LOGI(kCat, "opening %s with %s", uri_.c_str(), backend_name_.c_str());
            controller_->open(uri_);
            for (size_t i = 0; i < subtitle_files_.size(); ++i) load_subtitles(subtitle_files_[i], kExternalSubtitleTrack + static_cast<int>(i));
        }
    }
    void on_hide(Ui &ui) override { ui.set_clear_color(std::nullopt); }

    bool on_back(Ui &) override {
        stop();
        return false;  // let the navigator pop
    }

    void stop() {
        if (controller_) {
            const auto s = controller_->snapshot();
            WNK_LOGI(kCat, "stopping at %s: %llu video, %llu audio packets, %llu full retries", clock_text(s.position_us).c_str(),
                     static_cast<unsigned long long>(s.video_packets), static_cast<unsigned long long>(s.audio_packets),
                     static_cast<unsigned long long>(s.full_retries));
            controller_->stop();
        }
    }

    // Leaving the app: pause and give the decoder back (a held pause would keep it).
    void pause_for_background() {
        if (controller_ == nullptr) return;
        const PlayState st = controller_->state();
        resume_on_return_ = st == PlayState::Playing || st == PlayState::Seeking;
        controller_->pause_and_release();
    }
    // Back in the foreground: carry on if it was playing when the app left (the decoder was released,
    // so there is no picture to hold); a pause the viewer chose stays paused.
    void return_from_background() {
        if (controller_ == nullptr || !resume_on_return_) return;
        resume_on_return_ = false;
        WNK_LOGI(kCat, "back in the foreground: resuming");
        controller_->play();
    }

 protected:
    bool on_action(Ui &ui, const wnk::input::ActionEvent &e) override {
        if (e.phase == Phase::Release) return false;
        show_controls();
        switch (e.action) {
            case Action::Confirm:
            case Action::PlayPause:
                if (e.phase == Phase::Press) controller_->toggle_pause();
                return true;
            case Action::Play: controller_->play(); return true;
            case Action::Pause: controller_->pause(); return true;
            case Action::Left:
            case Action::Rewind: seek_by(ui, -10000000); return true;
            case Action::Right:
            case Action::FastForward: seek_by(ui, 10000000); return true;
            case Action::Stop: {
                wnk::input::ActionEvent back;
                back.action = Action::Back;
                return ui.handle(back);
            }
            case Action::Down:
                if (e.phase == Phase::Press) next_subtitles();
                return true;
            case Action::Up:
                if (e.phase == Phase::Press) open_tracks(ui);
                return true;
            default: return false;
        }
    }

    void update(Ui &ui, float) override {
        if (!controller_) return;
        if (opened_ms_ < 0) opened_ms_ = ui.now_ms();
        while (next_seek_ < seeks_.size() && ui.now_ms() - opened_ms_ >= seeks_[next_seek_].first * 1000) {
            WNK_LOGI(kCat, "scripted seek %+.0f s", seeks_[next_seek_].second);
            seek_by(ui, static_cast<int64_t>(seeks_[next_seek_].second * 1e6));  // as a key would
            ++next_seek_;
        }
        while (next_audio_ < audio_switches_.size() && ui.now_ms() - opened_ms_ >= audio_switches_[next_audio_].first * 1000) {
            const int track = static_cast<int>(audio_switches_[next_audio_].second);
            WNK_LOGI(kCat, "scripted audio track %d", track);
            controller_->set_audio_track(track);
            ++next_audio_;
        }
        while (next_speed_ < speeds_.size() && ui.now_ms() - opened_ms_ >= speeds_[next_speed_].first * 1000) {
            WNK_LOGI(kCat, "scripted speed %.2fx", speeds_[next_speed_].second);
            controller_->set_speed(speeds_[next_speed_].second);
            ++next_speed_;
        }
        while (adaptive_ && next_cap_ < caps_.size() && ui.now_ms() - opened_ms_ >= caps_[next_cap_].first * 1000) {
            const int64_t bps = static_cast<int64_t>(caps_[next_cap_].second * 1000);
            WNK_LOGI(kCat, "scripted video cap %lld kb/s", static_cast<long long>(bps / 1000));
            adaptive_->set_bandwidth_limit(bps);
            ++next_cap_;
        }
        const PlaybackSnapshot s = controller_->snapshot();
        const bool busy = s.state == PlayState::Opening || s.state == PlayState::Seeking || s.buffering;
        // The screensaver may start when paused, ended or failed, never while the video moves.
        if (busy || s.state == PlayState::Playing) {
            if (!screen_on_.held()) screen_on_ = ctx_.keep_screen_on("video playing");
        } else if (screen_on_.held()) {
            screen_on_.release();
        }
        spinner_->set_visible(busy);
        status_->set_text(s.state == PlayState::Error ? "Error" : s.buffering ? "Buffering" : play_state_name(s.state));
        // A stream without a duration (live DASH/HLS) shows LIVE instead.
        const bool live = s.duration_us == kNoTime && (s.info.container == "dash" || s.info.container == "hls");
        time_->set_text(clock_text(s.position_us) + " / " + (live ? std::string("LIVE") : clock_text(s.duration_us)) +
                        (s.position_estimated ? "" : " ·"));
        if (s.duration_us > 0 && s.position_us != kNoTime) progress_->set_value(static_cast<float>(s.position_us) / s.duration_us);
        show_preview(ui, s);
        if (s.state == PlayState::Error) detail_->set_text(s.error);
        if (s.audio_track != audio_track_) {
            audio_track_ = s.audio_track;
            const TrackInfo *t = s.info.track(audio_track_);
            const std::string label = t != nullptr ? "Audio: " + track_name(*t) : std::string();
            audio_label_->set_text(label);
            if (t != nullptr) {
                WNK_LOGI(kCat, "audio: track %d, %s", audio_track_, label.c_str());
                describe(s);
            }
        }
        // Controls hide during playback; paused, ended or failed keeps them up.
        const bool keep = s.state != PlayState::Playing;
        const bool visible = keep || ui.now_ms() - last_input_ms_ < 4000;
        bar_->set_opacity(visible ? 1.0f : 0.0f);
        subs_view_->set_bottom_margin(visible ? 0.30f : 0.07f);  // clear of the controls while they show
        std::string shown;
        for (const auto &l : subs_view_->lines()) shown += (shown.empty() ? "" : " / ") + l;
        if (shown != shown_) {
            shown_ = shown;
            if (!shown.empty()) WNK_LOGI(kCat, "subtitle shown: %s", shown.c_str());
        }
        if (const AssRenderer *ass = subs_view_->ass_renderer(); ass != nullptr) {
            const AssRenderer::Stats st = ass->stats();
            if (st.frames != ass_frames_logged_ && ui.now_ms() - last_ass_log_ms_ >= 2000) {
                last_ass_log_ms_ = ui.now_ms();
                ass_frames_logged_ = st.frames;
                WNK_LOGI(kCat, "ass: %llu frames rendered, last %.1f ms, max %.1f ms, %zu events",
                         static_cast<unsigned long long>(st.frames), st.last_ms, st.max_ms, st.events);
            }
        }
        // Once a second: what the controller has fed, for the PTS-unit experiment.
        if (ui.now_ms() - last_log_ms_ >= 1000 && s.state == PlayState::Playing) {
            last_log_ms_ = ui.now_ms();
            WNK_LOGI(kCat, "position %s (%s) last video pts %.2f s, last audio pts %.2f s, fed %llu video %llu audio, "
                     "full retries %llu, untimed %llu",
                     clock_text(s.position_us).c_str(), s.position_estimated ? "estimated" : "decoder",
                     s.last_video_pts_us == kNoTime ? -1.0 : s.last_video_pts_us / 1e6,
                     s.last_audio_pts_us == kNoTime ? -1.0 : s.last_audio_pts_us / 1e6,
                     static_cast<unsigned long long>(s.video_packets), static_cast<unsigned long long>(s.audio_packets),
                     static_cast<unsigned long long>(s.full_retries), static_cast<unsigned long long>(s.untimed_packets));
        }
        // Every 5 s: what the panel is doing with the picture (sdr, hdr, dolbyHdr), which a log of what
        // was declared cannot show. settingsservice reports it in the reply's "dimension" (PlxNative).
        if (ui.now_ms() - last_panel_ms_ >= 5000 && s.state == PlayState::Playing) {
            last_panel_ms_ = ui.now_ms();
            ctx_.pool().submit([](const wnk::async::CancellationToken &) {
                const auto r = wnk::platform::luna_call_sync("luna://com.webos.settingsservice/getSystemSettings",
                                                             R"({"category":"picture","keys":["pictureMode"]})", 2000);
                const auto j = nlohmann::json::parse(r.payload.empty() ? "{}" : r.payload, nullptr, false);
                std::string range = "?", mode = "?";
                if (j.is_object() && j.contains("dimension") && j["dimension"].is_object()) {
                    range = j["dimension"].value("dynamicRange", range);
                }
                if (j.is_object() && j.contains("settings") && j["settings"].is_object()) {
                    mode = j["settings"].value("pictureMode", mode);
                }
                WNK_LOGI(kCat, "panel: dynamic range %s, picture mode %s", range.c_str(), mode.c_str());
            });
        }
    }

 private:
    void show_controls() { last_input_ms_ = ctx_.ui().now_ms(); }

    static std::string track_name(const TrackInfo &t) {
        std::string name = !t.title.empty() ? t.title : !t.language.empty() ? t.language : "track " + std::to_string(t.index);
        name += " (" + t.codec_label;
        if (t.type == TrackType::Audio && t.channels > 0) name += ", " + std::to_string(t.channels) + " ch";
        return name + ")";
    }

    // The track menu: every audio track, then Off and every subtitle track this build can draw. The
    // current choices are marked and the playing audio track has focus.
    void open_tracks(Ui &ui) {
        if (tracks_.empty()) return;  // not open yet
        auto d = std::make_unique<Dialog>("Audio, subtitles and speed");
        Dialog *dialog = d.get();
        Button *current = nullptr;
        auto &body = d->body();
        body.add<Label>("Audio", TextRole::Small).set_muted(true);
        auto &audio = body.add<Grid>(2, 16);  // the dialog is 920 wide; DASH/HLS can list six languages
        const PlaybackSnapshot s = controller_->snapshot();
        for (const TrackInfo &t : tracks_) {
            if (t.type != TrackType::Audio) continue;
            const bool on = t.index == s.audio_track;
            auto &b = audio.add<Button>(track_name(t) + (on ? "  ·  playing" : ""), [this, dialog, index = t.index](Ui &u) {
                WNK_LOGI(kCat, "tracks: audio %d chosen", index);
                controller_->set_audio_track(index);
                dialog->dismiss(u);
            });
            b.name = "tracks.audio." + std::to_string(t.index);
            b.set_width(Dim::fill());
            if (on) current = &b;
        }
        body.add<Label>("Subtitles", TextRole::Small).set_muted(true);
        auto &subs = body.add<Grid>(2, 16);
        auto add_sub = [&](int index, const std::string &text) {
            const bool on = index == controller_->subtitle_track();
            auto &b = subs.add<Button>(text + (on ? "  ·  on" : ""), [this, dialog, index](Ui &u) {
                select_subtitles(index);
                dialog->dismiss(u);
            });
            b.name = "tracks.subs." + std::to_string(index);
            b.set_width(Dim::fill());
        };
        add_sub(-1, "Off");
        for (const TrackInfo &t : tracks_) {
            if (t.type == TrackType::Subtitle && t.subtitle != SubtitleKind::None) add_sub(t.index, track_name(t));
        }
        // Playback speed, done in the app (PlaybackController::set_speed): the speeds Plex offers.
        body.add<Label>("Speed", TextRole::Small).set_muted(true);
        auto &speeds = body.add<Grid>(4, 16);
        for (const double sp : {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0}) {
            char text[32];
            if (sp == 1.0) {
                std::snprintf(text, sizeof text, "Normal%s", s.speed == 1.0 ? "  ·  on" : "");
            } else {
                std::snprintf(text, sizeof text, "%gx%s", sp, s.speed == sp ? "  ·  on" : "");
            }
            auto &b = speeds.add<Button>(text, [this, dialog, sp](Ui &u) {
                WNK_LOGI(kCat, "tracks: speed %.2fx chosen", sp);
                controller_->set_speed(sp);
                dialog->dismiss(u);
            });
            b.name = "tracks.speed." + std::to_string(static_cast<int>(sp * 100));
            b.set_width(Dim::fill());
        }
        d->add_button("Close").name = "tracks.close";
        ui.show_dialog(std::move(d));
        if (current != nullptr) ui.focus(current);
        View *f = ui.focused();
        WNK_LOGI(kCat, "tracks menu open, focus=%s", f != nullptr ? f->name.c_str() : "<none>");
    }

    // The app's own faces, so a script asking for its family in bold gets the bold one. Read once.
    const std::vector<FontAttachment> &app_fonts() {
        if (app_fonts_.empty()) {
            for (const char *name : {"AtkinsonHyperlegible-Regular.ttf", "AtkinsonHyperlegible-Bold.ttf"}) {
                std::vector<uint8_t> data;
                if (FILE *f = std::fopen((ctx_.launch().app_dir + "/fonts/" + name).c_str(), "rb")) {
                    uint8_t buf[65536];
                    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
                    std::fclose(f);
                }
                if (!data.empty()) app_fonts_.push_back({name, std::make_shared<const std::vector<uint8_t>>(std::move(data))});
            }
        }
        return app_fonts_;
    }

    // A subtitle file beside the media: read whole on a worker, then added as a track and shown if
    // nothing else is.
    // Seek previews: the stream's own (DASH/HLS) or a WebVTT thumbnails file from the launch params.
    void load_thumbnails() {
        if (thumbs_requested_) return;
        thumbs_requested_ = true;
        if (adaptive_ == nullptr && thumbs_url_.empty()) return;
        struct Loaded {
            bool ok = false;
            ThumbnailTrack track;
            std::string error;
        };
        wnk::async::run_then_post<Loaded>(
            ctx_.pool(), ctx_.dispatcher(), life_.weak(), {},
            [adaptive = adaptive_, url = thumbs_url_, http = http_](const wnk::async::CancellationToken &cancel) {
                Loaded l;
                if (!url.empty()) {
                    if (!http) {
                        l.error = "no HTTP transport";
                        return l;
                    }
                    wnk::net::HttpRequest req;
                    req.url = url;
                    req.max_response_bytes = 4u << 20;
                    const wnk::net::HttpResponse r = http->perform(req, cancel);
                    if (!r.ok()) {
                        l.error = r.error != wnk::net::NetError::None ? wnk::net::error_name(r.error) : "HTTP " + std::to_string(r.status);
                        return l;
                    }
                    l.ok = parse_thumbnail_vtt(r.body, r.effective_url.empty() ? url : r.effective_url, &l.track, &l.error);
                } else {
                    l.ok = adaptive->thumbnails(kPreviewWidth, &l.track, cancel, &l.error);
                }
                return l;
            },
            [this](Loaded l) {
                if (!l.ok) {
                    WNK_LOGI(kCat, "seek previews: none (%s)", l.error.c_str());
                    return;
                }
                WNK_LOGI(kCat, "seek previews: %zu sheets, tiles %dx%d (%s)", l.track.sheets.size(), l.track.tile_width,
                         l.track.tile_height, l.track.id.c_str());
                thumbs_ = std::move(l.track);
            });
    }

    // A relative seek. The preview follows its target, counted from the last target while seeks come
    // in a row: the reported position may still show the old place for a moment (seen on a C1).
    void seek_by(Ui &ui, int64_t delta_us) {
        const PlaybackSnapshot s = controller_->snapshot();
        const bool row = ui.now_ms() - seek_ms_ < 2000 && preview_target_us_ != kNoTime;
        const int64_t base = row ? preview_target_us_ : s.position_us == kNoTime ? 0 : s.position_us;
        preview_target_us_ = std::max<int64_t>(0, base + delta_us);
        if (s.duration_us > 0) preview_target_us_ = std::min(preview_target_us_, s.duration_us);
        seek_ms_ = ui.now_ms();
        show_controls();
        controller_->seek_relative(delta_us);
    }

    // While seeking (and a moment after), the tile for the target over its place on the bar.
    void show_preview(Ui &ui, const PlaybackSnapshot &s) {
        const bool seeking = ui.now_ms() - seek_ms_ < 2000 && preview_target_us_ != kNoTime;
        const auto tile = !thumbs_.empty() && seeking && s.duration_us > 0 ? thumbs_.at(preview_target_us_) : std::nullopt;
        preview_row_->set_visible(tile.has_value());
        if (!tile) return;
        preview_->set_url(tile->url).set_region(tile->x, tile->y, tile->width, tile->height);
        preview_time_->set_text(clock_text(preview_target_us_));
        const float frac = std::clamp(static_cast<float>(preview_target_us_) / static_cast<float>(s.duration_us), 0.0f, 1.0f);
        const float track = 1920.0f - 160.0f;  // the bar's padding
        const float box = kPreviewWidth + 12.0f;
        preview_row_->set_padding({std::clamp(frac * track - box / 2, 0.0f, track - box), 0, 0, 0});
        const std::string shown = tile->url + " tile " + std::to_string(tile->x) + "," + std::to_string(tile->y) + " " +
                                  std::to_string(tile->width) + "x" + std::to_string(tile->height);
        if (shown != preview_shown_) {
            preview_shown_ = shown;
            WNK_LOGI(kCat, "seek preview %s for %s", shown.c_str(), clock_text(preview_target_us_).c_str());
        }
    }

    void load_subtitles(const std::string &url, int track) {
        FfmpegSourceOptions o;
        o.http = http_;
        struct Loaded {
            bool ok = false;
            SubtitleFile file;
            std::string error;
        };
        wnk::async::run_then_post<Loaded>(
            ctx_.pool(), ctx_.dispatcher(), life_.weak(), {},
            [url, track, o](const wnk::async::CancellationToken &cancel) {
                Loaded l;
                l.ok = load_subtitle_file(url, track, o, &l.file, &l.error, [&cancel] { return cancel.cancelled(); });
                return l;
            },
            [this, url](Loaded l) {
                if (!l.ok) {
                    WNK_LOGW(kCat, "subtitle file %s: %s", url.c_str(), l.error.c_str());
                    return;
                }
                const std::string path = url.substr(0, url.find_first_of("?#"));
                l.file.info.title = path.substr(path.find_last_of('/') + 1);
                const int index = l.file.info.index;
                WNK_LOGI(kCat, "subtitle file %s: %zu cues as track %d", l.file.info.title.c_str(), l.file.cues.size(), index);
                controller_->add_subtitle_track(l.file.info, std::move(l.file.cues));
                tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [index](const TrackInfo &t) { return t.index == index; }),
                              tracks_.end());
                tracks_.push_back(l.file.info);
                if (controller_->subtitle_track() < 0) select_subtitles(index);
            });
    }

    // Off → each subtitle track this build can draw → off.
    void next_subtitles() {
        std::vector<int> order{-1};
        for (const TrackInfo &t : tracks_) {
            if (t.type == TrackType::Subtitle && t.subtitle != SubtitleKind::None) order.push_back(t.index);
        }
        const auto cur = std::find(order.begin(), order.end(), controller_->subtitle_track());
        select_subtitles(cur == order.end() || cur + 1 == order.end() ? order.front() : *(cur + 1));
    }
    void select_subtitles(int track) {
        controller_->set_subtitle_track(track);
        std::string label = "Subtitles off";
        subs_view_->clear_ass_track();
        for (const TrackInfo &t : tracks_) {
            if (t.index != track) continue;
            label = "Subtitles: " + track_name(t);
            if (!t.ass_header.empty()) {
                // Styled: the media's fonts first, the app's own for any family it lacks.
                const PlaybackSnapshot snap = controller_->snapshot();
                const TrackInfo *v = snap.info.best(TrackType::Video);
                std::vector<FontAttachment> fonts = snap.info.fonts;
                fonts.insert(fonts.end(), app_fonts().begin(), app_fonts().end());
                const bool styled = subs_view_->set_ass_track(t.ass_header, fonts,
                                                              ctx_.launch().app_dir + "/fonts/AtkinsonHyperlegible-Regular.ttf",
                                                              v != nullptr ? v->width : 0, v != nullptr ? v->height : 0);
                WNK_LOGI(kCat, "subtitles: ASS %s, %zu embedded font(s)", styled ? "styled by libass" : "as plain text",
                         snap.info.fonts.size());
            }
        }
        subs_label_->set_text(label);
        WNK_LOGI(kCat, "subtitles: track %d, %s", track, label.c_str());
    }
    void describe(const PlaybackSnapshot &s) {
        std::string d = s.info.container;
        if (s.format.video) {
            d += "  ·  " + std::string(codec_name(s.format.video->codec)) + " " + std::to_string(s.format.video->width) + "x" +
                 std::to_string(s.format.video->height);
        }
        if (const TrackInfo *t = s.info.track(s.audio_track); t != nullptr && s.format.audio) {
            d += "  ·  " + t->codec_label + " " + std::to_string(t->channels) + " ch";
            if (s.format.audio->codec == CodecId::PcmS16) d += " decoded to PCM stereo";
        }
        d += "  ·  " + std::string(controller_->backend_name());
        detail_->set_text(d);
    }

    wnk::app::Context &ctx_;
    std::string uri_;
    SubtitleView *subs_view_ = nullptr;
    Label *subs_label_ = nullptr;
    Label *audio_label_ = nullptr;
    int audio_track_ = -1;  // as last shown
    std::vector<TrackInfo> tracks_;
    std::string shown_;
    bool resume_on_return_ = false;  // playing when the app went to the background
    wnk::app::WakeLock screen_on_;   // held while the video moves
    std::string backend_name_;
    std::unique_ptr<PlaybackController> controller_;
    Column *bar_;
    Label *title_, *status_, *time_, *detail_;
    ProgressBar *progress_;
    Row *preview_row_ = nullptr;
    ImageView *preview_ = nullptr;
    Label *preview_time_ = nullptr;
    std::string thumbs_url_;
    bool thumbs_requested_ = false;
    ThumbnailTrack thumbs_;
    double seek_ms_ = -1e9;
    int64_t preview_target_us_ = kNoTime;
    std::string preview_shown_;
    Spinner *spinner_;
    bool opened_ = false;
    double last_input_ms_ = 0;
    double last_log_ms_ = 0;
    double last_panel_ms_ = 0;
    double opened_ms_ = -1;
    std::vector<std::pair<double, double>> seeks_;
    size_t next_seek_ = 0;
    std::vector<std::pair<double, double>> audio_switches_;
    size_t next_audio_ = 0;
    std::vector<std::pair<double, double>> speeds_;
    size_t next_speed_ = 0;
    std::vector<std::pair<double, double>> caps_;
    size_t next_cap_ = 0;
    std::vector<std::string> subtitle_files_;
    std::vector<FontAttachment> app_fonts_;
    uint64_t ass_frames_logged_ = 0;
    double last_ass_log_ms_ = 0;
    std::shared_ptr<wnk::net::HttpTransport> http_;
    AdaptiveSource *adaptive_ = nullptr;  // owned by controller_; null for files
    wnk::async::Lifetime life_;  // last: worker results for a closed screen are dropped
};

// ---- experiment: DRM through the platform pipeline -----------------------------------------------

// Widevine and PlayReady never give the app a key, so they cannot go through the kit's player. This
// screen drives LG's own path instead and logs every reply (docs/research/DRM-FEASIBILITY.md):
//   SDL exported window → drm/load → drm/sendDrmMessage (licence initiator) →
//   com.webos.media/load {uri, option.drm.clientId, windowId} → subscribe → play
// Nothing here is the kit's API; it is an experiment to learn whether the path works at all.
struct DrmTest {
    std::string uri = "https://test.playready.microsoft.com/media/profficialsite/tearsofsteel_4k.ism/manifest.mpd";
    std::string drm = "playready";
    std::string license_url = "https://test.playready.microsoft.com/service/rightsmanager.asmx?cfg=(persist:false,sl:150)";
    std::string transport = "MPEG-DASH";  // mediaTransportType
    std::string media_type = "media";     // the load call's "type"
    // How the licence is obtained:
    //   "service"   LG's DRM service first (drm/load, sendDrmMessage), its clientId in option.drm
    //   "url"       option.drm {type, licenseUrl, useLicenseCallback:false}: the pipeline fetches it
    //   "callback"  option.drm {type, useLicenseCallback:true}: the pipeline raises drmEncrypted with a
    //               challenge, the app POSTs it to `la` and hands the licence back with setDrmOperation
    //               (what EME's message/update do; LG's video_player_drm Flutter plugin does the same)
    std::string license = "service";
    bool transport_in_option = true;      // mediaTransportType inside option (else beside it, as the plugin)
};

class DrmTestScreen : public Screen {
 public:
    DrmTestScreen(wnk::app::Context &ctx, DrmTest test, std::shared_ptr<wnk::net::HttpTransport> http)
        : Screen("DRM experiment"), ctx_(ctx), test_(std::move(test)), http_(std::move(http)) {
        name = "drmtest";
        auto &col = add<Column>(14);
        col.set_align(Align::Stretch, Align::End);
        col.set_padding({80, 40, 80, 60});
        col.set_background(wnk::render::Color::hex(0x000000B0));
        col.add<Label>("DRM experiment: " + test_.drm + " through the platform pipeline", TextRole::Title);
        col.add<Label>(test_.uri, TextRole::Small).set_muted(true);
        drm_ = &col.add<Label>("DRM: starting");
        media_ = &col.add<Label>("Media: waiting");
        event_ = &col.add<Label>("", TextRole::Small);
        event_->set_muted(true).set_max_lines(2);
        col.add<Label>("Back stops", TextRole::Small).set_muted(true);
        st_ = std::make_shared<State>();
    }

    void on_show(Ui &ui) override {
        ui.set_clear_color(wnk::render::kTransparent);
        if (started_) return;
        started_ = true;
        screen_on_ = ctx_.keep_screen_on("platform playback");
        // The video window, as the Starfish backend makes it (SDL-webOS exports it to the media stack).
        using CreateFn = const char *(*)(int);
        using SetFn = int (*)(const char *, void *, void *);
        auto create = reinterpret_cast<CreateFn>(dlsym(RTLD_DEFAULT, "SDL_webOSCreateExportedWindow"));
        auto set = reinterpret_cast<SetFn>(dlsym(RTLD_DEFAULT, "SDL_webOSSetExportedWindow"));
        if (create != nullptr) {
            if (const char *id = create(0)) st_->window_id = id;
        }
        if (set != nullptr && !st_->window_id.empty()) {
            struct { int x, y, w, h; } src{0, 0, 1920, 1080}, dst{0, 0, 1920, 1080};
            WNK_LOGI(kCat, "drmtest: exported window %s, set rv=%d", st_->window_id.c_str(), set(st_->window_id.c_str(), &src, &dst));
        } else {
            WNK_LOGW(kCat, "drmtest: no exported window (the picture will not be visible)");
        }
        auto st = st_;
        auto &main = ctx_.dispatcher();
        const DrmTest t = test_;
        const std::string app_id = ctx_.launch().app_id;
        auto post = [&main, st](std::function<void(State &)> fn) {
            main.post([st, fn = std::move(fn)] {
                if (st->screen_alive) fn(*st);
            });
        };
        auto http = http_;
        ctx_.pool().submit([st, t, app_id, post, http](const wnk::async::CancellationToken &) {
            using wnk::platform::luna_call_sync;
            using nlohmann::json;
            auto log = [](const char *what, const wnk::platform::LunaReply &r) {
                WNK_LOGI(kCat, "drmtest: %s -> %s %s %s", what, wnk::platform::luna_status_name(r.status), r.payload.c_str(), r.detail.c_str());
                return json::parse(r.payload.empty() ? "{}" : r.payload, nullptr, false);
            };
            std::string client, err;
            if (t.drm != "none" && t.license == "service") {
            const json loaded = log("drm/load", luna_call_sync("luna://com.webos.service.drm/load",
                                                                json{{"drmType", t.drm}, {"appId", app_id}}.dump(), 8000));
            if (!loaded.is_object() || !loaded.value("returnValue", false) || !loaded.contains("clientId")) {
                post([](State &s) { s.drm_text = "DRM: load refused (see the log)"; });
                return;
            }
            client = loaded["clientId"].get<std::string>();
            {
                std::lock_guard<std::mutex> lock(st->m);
                st->client_id = client;
            }
            auto rights = wnk::platform::luna_subscribe(
                "luna://com.webos.service.drm/getRightsError", json{{"clientId", client}, {"subscribe", true}}.dump(),
                [](const std::string &r) { WNK_LOGI(kCat, "drmtest: rights event %s", r.c_str()); }, true, &err);
            if (!rights) WNK_LOGW(kCat, "drmtest: getRightsError subscribe failed: %s", err.c_str());
            if (!st->keep(std::move(rights))) return;
            if (t.drm == "playready") {
                const std::string initiator =
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?><PlayReadyInitiator xmlns=\"http://schemas.microsoft.com/DRM/2007/03/protocols/\">"
                    "<LicenseServerUriOverride><LA_URL>" + t.license_url + "</LA_URL></LicenseServerUriOverride></PlayReadyInitiator>";
                const json sent = log("drm/sendDrmMessage", luna_call_sync("luna://com.webos.service.drm/sendDrmMessage",
                    json{{"clientId", client}, {"msgType", "application/vnd.ms-playready.initiator+xml"}, {"msg", initiator},
                         {"drmSystemId", "urn:dvb:casystemid:19219"}}.dump(), 15000));
                const bool ok = sent.is_object() && sent.value("returnValue", false);
                post([ok, sent](State &s) {
                    s.drm_text = std::string("DRM: client loaded, licence initiator ") + (ok ? "accepted" : "refused: " + sent.dump());
                });
            } else {
                post([](State &s) { s.drm_text = "DRM: client loaded (no licence message for this type)"; });
            }
            }  // drm != none
            json option = {{"appId", app_id}};
            if (!t.transport.empty() && t.transport_in_option) option["mediaTransportType"] = t.transport;
            if (t.drm != "none" && t.license == "service") option["drm"] = {{"type", t.drm}, {"clientId", client}};
            if (t.drm != "none" && t.license == "url") {
                option["drm"] = {{"type", t.drm}, {"licenseUrl", t.license_url}, {"useLicenseCallback", false}};
            }
            if (t.drm != "none" && t.license == "callback") option["drm"] = {{"type", t.drm}, {"useLicenseCallback", true}};
            if (t.license != "service") post([t](State &s) { s.drm_text = "DRM: licence by " + t.license; });
            if (!st->window_id.empty()) option["windowId"] = st->window_id;
            json payload = {{"option", option}};
            if (!t.transport.empty() && !t.transport_in_option) payload["mediaTransportType"] = t.transport;
            const json load = {{"uri", t.uri}, {"type", t.media_type}, {"payload", payload}};
            const json media = log("media/load", luna_call_sync("luna://com.webos.media/load", load.dump(), 15000));
            if (!media.is_object() || !media.value("returnValue", false) || !media.contains("mediaId")) {
                post([media](State &s) { s.media_text = "Media: load refused: " + media.dump(); });
                return;
            }
            const std::string media_id = media["mediaId"];
            {
                std::lock_guard<std::mutex> lock(st->m);
                st->media_id = media_id;
            }
            auto events = wnk::platform::luna_subscribe(
                "luna://com.webos.media/subscribe", json{{"mediaId", media_id}}.dump(),
                [st, post, t, http, media_id](const std::string &r) {
                    const json e = json::parse(r, nullptr, false);
                    if (e.is_object() && e.contains("drmEncrypted") && e["drmEncrypted"].is_object()) {
                        // EME's "message": a licence challenge. Answer it off this (libhelpers) thread.
                        WNK_LOGI(kCat, "drmtest: drmEncrypted %s", r.substr(0, 400).c_str());
                        post([](State &s) { s.drm_text = "DRM: challenge received, fetching the licence"; });
                        std::thread([d = e["drmEncrypted"], t, http, media_id, post] { answer_challenge(d, t, http, media_id, post); }).detach();
                        return;
                    }
                    const bool time = e.is_object() && e.contains("currentTime");
                    if (time) {
                        // {"currentTime": ms} or {"currentTime": {"currentTime": ms, "mediaId": ...}}
                        const json &c = e["currentTime"];
                        const int64_t ms = c.is_number() ? c.get<int64_t>()
                                           : c.is_object() && c.contains("currentTime") && c["currentTime"].is_number()
                                               ? c["currentTime"].get<int64_t>()
                                               : -1;
                        const int64_t prev = st->last_logged_ms.exchange(ms);
                        if (ms >= 0 && (prev < 0 || ms / 1000 != prev / 1000)) {
                            WNK_LOGI(kCat, "drmtest: currentTime %lld ms", static_cast<long long>(ms));
                        }
                        post([ms](State &s) { s.media_text = "Media: playing, " + std::to_string(ms / 1000) + " s"; });
                    } else {
                        WNK_LOGI(kCat, "drmtest: media event %s", r.c_str());
                        post([r](State &s) { s.event_text = r.substr(0, 240); });
                    }
                }, true, &err);
            if (!events) WNK_LOGW(kCat, "drmtest: media subscribe failed: %s", err.c_str());
            if (!st->keep(std::move(events))) return;
            post([](State &s) { s.media_text = "Media: loaded, starting"; });
            log("media/play", luna_call_sync("luna://com.webos.media/play", json{{"mediaId", media_id}}.dump(), 8000));
        });
    }

    void on_hide(Ui &ui) override { ui.set_clear_color(std::nullopt); }

    bool on_back(Ui &) override {
        stop();
        return false;
    }

    // POST the challenge to the licence server and give the answer to the pipeline:
    // com.webos.media/setDrmOperation {mediaId, systemId, drmData, drmMsgType "update", service, kid,
    // sessionId, requestId} (the fields libumedia_api's uMediaClient::setDrmOperation sends).
    template <typename Post>
    static void answer_challenge(const nlohmann::json &d, const DrmTest &t, const std::shared_ptr<wnk::net::HttpTransport> &http,
                                 const std::string &media_id, Post post) {
        using nlohmann::json;
        auto str = [&d](const char *k) { return d.contains(k) && d[k].is_string() ? d[k].get<std::string>() : std::string(); };
        const auto challenge = wnk::storage::base64_decode(str("drmData"));
        if (!challenge || !http) {
            WNK_LOGW(kCat, "drmtest: challenge not base64 or no HTTP transport");
            return;
        }
        wnk::net::HttpRequest req;
        req.method = "POST";
        req.url = t.license_url;
        req.body = *challenge;
        if (t.drm == "playready") {
            req.headers = {{"Content-Type", "text/xml; charset=utf-8"},
                           {"SOAPAction", "\"http://schemas.microsoft.com/DRM/2007/03/protocols/AcquireLicense\""}};
        } else {
            req.headers = {{"Content-Type", "application/octet-stream"}};
        }
        const wnk::net::HttpResponse res = http->perform(req, {});
        WNK_LOGI(kCat, "drmtest: licence server %ld, %zu bytes (challenge %zu bytes, msgType %s)", res.status, res.body.size(),
                 challenge->size(), str("drmMsgType").c_str());
        if (!res.ok()) {
            post([res](State &s) { s.drm_text = "DRM: licence server answered " + std::to_string(res.status); });
            return;
        }
        int64_t request_id = 0;
        if (d.contains("requestId") && d["requestId"].is_number_integer()) request_id = d["requestId"].get<int64_t>();
        const json op = {{"mediaId", media_id}, {"systemId", str("systemId")}, {"drmData", wnk::storage::base64_encode(res.body)},
                         {"drmMsgType", "update"}, {"service", str("service")}, {"kid", str("key")},
                         {"sessionId", str("sessionId")}, {"requestId", request_id}};
        const auto reply = wnk::platform::luna_call_sync("luna://com.webos.media/setDrmOperation", op.dump(), 8000);
        WNK_LOGI(kCat, "drmtest: setDrmOperation -> %s %s %s", wnk::platform::luna_status_name(reply.status), reply.payload.c_str(),
                 reply.detail.c_str());
        post([reply](State &s) { s.drm_text = "DRM: licence handed to the pipeline: " + reply.payload.substr(0, 120); });
    }

    void stop() {
        if (!st_ || !st_->screen_alive) return;
        st_->screen_alive = false;
        {
            // Closed now, on the main thread: no event may reach the dispatcher after the app is gone.
            std::lock_guard<std::mutex> lock(st_->m);
            st_->stopped = true;
            for (auto &sub : st_->subs) sub->close();
        }
        auto st = st_;
        // Teardown off the main thread (Luna calls block); the state outlives the screen.
        std::thread([st] {
            std::string media, client;
            {
                std::lock_guard<std::mutex> lock(st->m);
                media = st->media_id;
                client = st->client_id;
            }
            if (!media.empty()) {
                const auto r = wnk::platform::luna_call_sync("luna://com.webos.media/unload", "{\"mediaId\":\"" + media + "\"}", 5000);
                WNK_LOGI(kCat, "drmtest: media/unload -> %s", r.payload.c_str());
            }
            if (!client.empty()) {
                const auto r = wnk::platform::luna_call_sync("luna://com.webos.service.drm/unload", "{\"clientId\":\"" + client + "\"}", 5000);
                WNK_LOGI(kCat, "drmtest: drm/unload -> %s", r.payload.c_str());
            }
            using DestroyFn = void (*)(const char *);
            auto destroy = reinterpret_cast<DestroyFn>(dlsym(RTLD_DEFAULT, "SDL_webOSDestroyExportedWindow"));
            if (destroy != nullptr && !st->window_id.empty()) destroy(st->window_id.c_str());
        }).detach();
    }

 protected:
    void update(Ui &, float) override {
        drm_->set_text(st_->drm_text);
        media_->set_text(st_->media_text);
        event_->set_text(st_->event_text);
    }

 private:
    struct State {
        std::mutex m;
        std::string window_id, client_id, media_id;  // ids guarded by m once the worker runs
        std::vector<std::unique_ptr<wnk::platform::LunaSubscription>> subs;  // guarded by m
        bool stopped = false;                                              // guarded by m
        std::atomic<int64_t> last_logged_ms{-1};
        // Keeps a subscription; after stop() it is closed at once. False when stopped.
        bool keep(std::unique_ptr<wnk::platform::LunaSubscription> sub) {
            std::lock_guard<std::mutex> lock(m);
            if (sub && stopped) sub->close();
            if (sub) subs.push_back(std::move(sub));
            return !stopped;
        }
        // Main thread only.
        bool screen_alive = true;
        std::string drm_text = "DRM: starting", media_text = "Media: waiting", event_text;
    };
    wnk::app::Context &ctx_;
    DrmTest test_;
    std::shared_ptr<wnk::net::HttpTransport> http_;
    std::shared_ptr<State> st_;
    Label *drm_, *media_, *event_;
    bool started_ = false;
    wnk::app::WakeLock screen_on_;
};

// ---- clip list -------------------------------------------------------------------------------------

class HomeScreen : public Screen {
 public:
    HomeScreen(wnk::app::Context &ctx, Navigator &nav, Settings settings, const std::vector<Clip> &clips,
               const std::vector<Stream> &streams)
        : Screen("home"), ctx_(ctx), nav_(nav), settings_(std::move(settings)) {
        name = "home";
        auto &scroll = add<ScrollView>();
        scroll.set_width(Dim::fill()).set_height(Dim::fill());
        auto &col = scroll.add<Column>(20);
        col.set_padding({140, 70, 140, 50});
        col.add<Label>("Native playback", TextRole::Title);
        col.add<Label>("Hardware decoding through LG's Starfish or NDL, demuxed by the kit's bundled FFmpeg. Each clip flashes "
                       "white and beeps at the start of every second: the flash and the beep should coincide.")
            .set_muted(true)
            .set_max_lines(3);
        auto &list = col.add<Column>(10);
        list.set_padding({0, 10, 0, 0});
        for (size_t i = 0; i < clips.size(); ++i) {
            const Clip c = clips[i];
            auto &b = list.add<Button>(c.title + "   —   " + c.detail, [this, c](Ui &ui) { play(ui, "file://" + settings_.clips_dir + "/" + c.file, c.title); });
            b.set_align(Align::Start, Align::Start);
            b.name = "clip" + std::to_string(i);
        }
        list.add<Label>("Streams (DASH / HLS over the network)", TextRole::Small).set_muted(true).set_padding({0, 12, 0, 0});
        for (size_t i = 0; i < streams.size(); ++i) {
            const Stream st = streams[i];
            auto &b = list.add<Button>(st.title + "   —   " + st.detail, [this, st](Ui &ui) { play(ui, st.url, st.title, st.keys); });
            b.set_align(Align::Start, Align::Start);
            b.name = "stream" + std::to_string(i);
        }
        list.add<Label>("Experiments", TextRole::Small).set_muted(true).set_padding({0, 12, 0, 0});
        list.add<Button>("PlayReady through the platform pipeline   —   Microsoft test stream, licence from LG's DRM service",
                         [this](Ui &ui) { drm_test(ui, DrmTest{}); })
            .set_align(Align::Start, Align::Start)
            .name = "drmtest";
        std::string info = std::string("Decoder: ") + (wnk::platform::kIsWebOS ? "NDL DirectMedia v2" : "none on the host (pacing backend, no picture)");
        info += "   ·   PTS unit: ";
        info += settings_.pts_unit == PtsUnit::Milliseconds ? "ms" : "µs";
        info += "   ·   " + FfmpegSource::version();
        col.add<Label>(info, TextRole::Small).set_muted(true).set_padding({0, 30, 0, 0});
    }

    void drm_test(Ui &ui, DrmTest t) {
        WNK_LOGI(kCat, "drm experiment: %s %s via %s", t.drm.c_str(), t.uri.c_str(), t.transport.c_str());
        nav_.push(ui, std::make_unique<DrmTestScreen>(ctx_, std::move(t), settings_.http));
    }

    void play(Ui &ui, const std::string &uri, const std::string &title, const std::string &keys = {}) {
        WNK_LOGI(kCat, "play %s%s", uri.c_str(), keys.empty() ? "" : " (ClearKey keys given)");
        Settings s = settings_;
        std::string err;
        if (!keys.empty() && !parse_content_keys(keys, &s.keys, &err)) WNK_LOGW(kCat, "%s", err.c_str());
        nav_.push(ui, std::make_unique<PlayerScreen>(ctx_, s, uri, title));
    }

 private:
    wnk::app::Context &ctx_;
    Navigator &nav_;
    Settings settings_;
};

// What launch parameters ask the player to open: a URI, a bundled clip or stream by index, or the
// DRM experiment. Read at launch and again on every relaunch.
struct OpenRequest {
    std::string uri;
    int autoplay = -1;
    std::optional<DrmTest> drm_test;
    bool any() const { return !uri.empty() || autoplay >= 0 || drm_test.has_value(); }
};

OpenRequest open_request(const nlohmann::json &params) {
    OpenRequest r;
    if (!params.is_object()) return r;
    if (params.contains("uri") && params["uri"].is_string()) r.uri = params["uri"];
    if (params.contains("drmTest") && params["drmTest"].is_object()) {
        const auto &d = params["drmTest"];
        r.drm_test = DrmTest{};
        r.drm_test->uri = d.value("uri", r.drm_test->uri);
        r.drm_test->drm = d.value("drm", r.drm_test->drm);
        r.drm_test->license_url = d.value("la", r.drm_test->license_url);
        r.drm_test->transport = d.value("transport", r.drm_test->transport);
        r.drm_test->media_type = d.value("type", r.drm_test->media_type);
        r.drm_test->license = d.value("license", r.drm_test->license);
        r.drm_test->transport_in_option = d.value("transportInOption", r.drm_test->transport_in_option);
    }
    if (params.contains("autoplay") && params["autoplay"].is_number_integer()) r.autoplay = params["autoplay"];
    return r;
}

class Player : public wnk::app::Delegate {
 public:
    void on_start(wnk::app::Context &ctx) override {
        Settings s;
        s.clips_dir = ctx.launch().app_dir + "/clips";
        std::string curl_error;
        s.http = wnk::net::make_curl_transport({}, &curl_error);
        if (!s.http) WNK_LOGW(kCat, "no HTTP streaming: %s", curl_error.c_str());
        const auto params = nlohmann::json::parse(ctx.launch().params_json.empty() ? "{}" : ctx.launch().params_json, nullptr, false);
        if (params.is_object()) {
            if (params.contains("ptsUnit") && params["ptsUnit"] == "ms") s.pts_unit = PtsUnit::Milliseconds;
            if (params.contains("ndlReload") && params["ndlReload"] == 1) s.ndl_reload = true;
            if (params.contains("backend") && params["backend"].is_string()) s.backend = params["backend"];
            if (params.contains("seekMode") && params["seekMode"] == "splice") s.native_seek = false;
            if (params.contains("pcmLeadMs") && params["pcmLeadMs"].is_number_integer()) {
                s.pcm_lead_ms = std::clamp(params["pcmLeadMs"].get<int>(), 0, 2000);
            }
            if (params.contains("leadMs") && params["leadMs"].is_number_integer()) {
                s.lead_ms = std::clamp(params["leadMs"].get<int>(), 100, 5000);
            }
            if (params.contains("ac3") && params["ac3"].is_boolean()) s.ac3 = params["ac3"];
            if (params.contains("lang") && params["lang"].is_string()) s.language = params["lang"];
            if (params.contains("keys") && params["keys"].is_string()) {
                std::string err;
                if (!parse_content_keys(params["keys"].get<std::string>(), &s.keys, &err)) WNK_LOGW(kCat, "%s", err.c_str());
            }
            if (params.contains("licenseUrl") && params["licenseUrl"].is_string()) s.license_url = params["licenseUrl"];
            // "at:value,at:value" with `at` in seconds after the player opens.
            auto timed = [&](const char *key, std::vector<std::pair<double, double>> *out) {
                if (!params.contains(key) || !params[key].is_string()) return;
                const std::string list = params[key];
                size_t i = 0;
                while (i < list.size()) {
                    double at = 0, value = 0;
                    if (std::sscanf(list.c_str() + i, "%lf:%lf", &at, &value) == 2) out->emplace_back(at, value);
                    const size_t comma = list.find(',', i);
                    if (comma == std::string::npos) break;
                    i = comma + 1;
                }
            };
            timed("seeks", &s.seeks);
            timed("audio", &s.audio_switches);
            timed("speeds", &s.speeds);
            timed("capKbps", &s.caps);
            if (params.contains("codecs") && params["codecs"].is_string()) s.codecs = params["codecs"];
            if (params.contains("thumbs") && params["thumbs"].is_string()) s.thumbs = params["thumbs"];
            if (params.contains("subs") && params["subs"].is_string()) s.subtitle_files.push_back(params["subs"]);
            if (params.contains("subs") && params["subs"].is_array()) {
                for (const auto &f : params["subs"]) {
                    if (f.is_string()) s.subtitle_files.push_back(f);
                }
            }
            if (params.contains("startKbps") && params["startKbps"].is_number_integer()) {
                s.start_bps = std::clamp<int64_t>(params["startKbps"].get<int64_t>(), 100, 1000000) * 1000;
            }
        }
        if (wnk::platform::kIsWebOS) {
            s.device = DeviceMediaCaps::detect();
            WNK_LOGI(kCat, "device media: %s", s.device.summary().c_str());
            WNK_LOGI(kCat, "audio output: %s", wnk::media::AudioOutputCaps::detect().summary().c_str());
        }
        WNK_LOGI(kCat, "%s; PTS unit %s; NDL pause/seek by %s; lead %d ms", FfmpegSource::version().c_str(),
                 s.pts_unit == PtsUnit::Milliseconds ? "ms" : "us", s.ndl_reload ? "reload" : "hold/splice", s.lead_ms);
        clips_ = {
            {"H.264 + AAC", "MP4 · 1080p25 · AAC as is (Starfish) or decoded to PCM (NDL)", "h264-aac.mp4"},
            {"HEVC + AC-3 5.1", "MKV · 1080p25 · AC-3 as is (Starfish) or downmixed to PCM stereo (NDL)", "hevc-ac3-5.1.mkv"},
            {"H.264 + MP3", "MPEG-TS · 1080p25 · MP3 re-encoded to AC-3 (Starfish) or decoded to PCM (NDL)", "h264-mp3.ts"},
            {"Audio and subtitle tracks", "MKV · H.264 · AAC English (1 kHz) and French (500 Hz) · SubRip English and French · Up",
             "h264-aac-subs.mkv"},
            {"H.264 + FLAC 7.1", "MKV · 1080p25 · 96 kHz 7.1 FLAC re-encoded to AC-3 5.1 48 kHz (Starfish)", "h264-flac-7.1.mkv"},
            {"H.264 + Opus", "MKV · 1080p25 · Opus re-encoded to AC-3 (Starfish) or as is (NDL)", "h264-opus.mkv"},
        };
        streams_ = {
            {"Dolby Vision + Atmos", "DASH · Dolby test stream · HEVC up to 2160p59.94 · E-AC-3 JOC",
             "https://media.developer.dolby.com/DolbyVision_Atmos/profile8.1_DASH/p8.1.mpd"},
            {"Apple bipbop", "HLS · fMP4 byte ranges · H.264/HEVC · separate AAC, AC-3, E-AC-3",
             "https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_adv_example_hevc/master.m3u8"},
            {"Mux test stream", "HLS · MPEG-TS · H.264 + AAC muxed", "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"},
            {"Shaka angel-one", "DASH · SegmentBase (sidx) · H.264 · AAC in six languages",
             "https://storage.googleapis.com/shaka-demo-assets/angel-one/dash.mpd"},
            {"DASH-IF livesim2 (live)", "DASH · dynamic · 2 s segments · 1 min time-shift window · live WebVTT (Down)",
             "https://livesim2.dashif.org/livesim2/timesubswvtt_en/testpic_2s/Manifest.mpd"},
            {"Unified Streaming (live)", "HLS · live · MPEG-TS · H.264 + AAC renditions",
             "https://demo.unified-streaming.com/k8s/live/stable/live.isml/.m3u8"},
            // ClearKey (Common Encryption with keys the app has): Shaka's published test key, and
            // livesim2's licence server named in the manifest.
            {"ClearKey: Shaka angel-one", "DASH · CENC 'cenc' · key given (kid:key)",
             "https://storage.googleapis.com/shaka-demo-assets/angel-one-clearkey/dash.mpd",
             "feedf00deedeadbeeff0baadf00dd00d:00112233445566778899aabbccddeeff"},
            {"ClearKey: livesim2 cenc (live)", "DASH · dynamic · CENC 'cenc' · key from the licence server",
             "https://livesim2.dashif.org/livesim2/eccp_cenc/testpic_2s/Manifest.mpd"},
            {"ClearKey: livesim2 cbcs (live)", "DASH · dynamic · CENC 'cbcs' · key from the licence server",
             "https://livesim2.dashif.org/livesim2/eccp_cbcs/testpic_2s/Manifest.mpd"},
            {"AES-128: JW Player oceans", "HLS · MPEG-TS segments encrypted AES-128 · key from the playlist's URI",
             "https://playertest.longtailvideo.com/adaptive/oceans_aes/oceans_aes.m3u8"},
            {"AES-128: Mux key rotation", "HLS · a new key and IV for every segment",
             "https://test-streams.mux.dev/dai-discontinuity-deltatre/manifest.m3u8"},
        };
        clips_dir_ = s.clips_dir;
        Ui &ui = ctx.ui();
        nav_ = &ui.root().add<Navigator>();
        home_ = &static_cast<HomeScreen &>(nav_->push(ui, std::make_unique<HomeScreen>(ctx, *nav_, s, clips_, streams_)));
        open(ui, open_request(params));
    }

    // Launched again while running (a deep link, a second ares-launch): whatever plays stops and the
    // new request opens from the home list. Playback settings (backend, lead, seeks) keep their
    // launch values. A relaunch without a request leaves everything as it is.
    void on_relaunch(wnk::app::Context &ctx, const std::string &params_json) override {
        const OpenRequest r = open_request(nlohmann::json::parse(params_json, nullptr, false));
        if (!r.any()) {
            WNK_LOGI(kCat, "relaunch: nothing to open, carrying on");
            return;
        }
        Ui &ui = ctx.ui();
        while (Dialog *d = ui.top_dialog()) ui.dismiss_dialog(d);  // the track menu points at the player
        if (auto *p = dynamic_cast<PlayerScreen *>(nav_->top())) p->stop();
        if (auto *d = dynamic_cast<DrmTestScreen *>(nav_->top())) d->stop();
        nav_->pop_to_root(ui);
        WNK_LOGI(kCat, "relaunch: opening the new request");
        open(ui, r);
    }

    void on_lifecycle(wnk::app::Context &, const wnk::app::Lifecycle::Transition &t) override {
        // Leaving the app: stop decoding, release NDL and tell it.
        if (t.to == wnk::app::State::Suspended) {
            if (auto *p = dynamic_cast<PlayerScreen *>(nav_->top())) p->pause_for_background();
            if (auto *d = dynamic_cast<DrmTestScreen *>(nav_->top())) d->stop();
            if (wnk::platform::kIsWebOS) NdlBackend::set_app_foreground(false);
        } else if (t.to == wnk::app::State::Resuming && wnk::platform::kIsWebOS) {
            NdlBackend::set_app_foreground(true);
        } else if (t.from == wnk::app::State::Resuming && t.to == wnk::app::State::Running) {
            if (auto *p = dynamic_cast<PlayerScreen *>(nav_->top())) p->return_from_background();
        }
    }

    void on_stop(wnk::app::Context &) override {
        if (auto *p = dynamic_cast<PlayerScreen *>(nav_ != nullptr ? nav_->top() : nullptr)) p->stop();
        if (auto *d = dynamic_cast<DrmTestScreen *>(nav_ != nullptr ? nav_->top() : nullptr)) d->stop();
    }

 private:
    void open(Ui &ui, const OpenRequest &r) {
        const int n_clips = static_cast<int>(clips_.size());
        if (r.drm_test) {
            home_->drm_test(ui, *r.drm_test);
        } else if (!r.uri.empty()) {
            home_->play(ui, r.uri, r.uri.substr(r.uri.find_last_of('/') + 1));
        } else if (r.autoplay >= 0 && r.autoplay < n_clips) {
            home_->play(ui, "file://" + clips_dir_ + "/" + clips_[r.autoplay].file, clips_[r.autoplay].title);
        } else if (r.autoplay >= n_clips && r.autoplay < n_clips + static_cast<int>(streams_.size())) {
            const Stream &st = streams_[static_cast<size_t>(r.autoplay - n_clips)];
            home_->play(ui, st.url, st.title, st.keys);
        }
    }

    Navigator *nav_ = nullptr;
    HomeScreen *home_ = nullptr;
    std::vector<Clip> clips_;
    std::vector<Stream> streams_;
    std::string clips_dir_;
};

}  // namespace

int main(int argc, char **argv) {
    wnk::app::AppConfig config;
    config.fallback_app_id = "org.wnk.examples.player";
    config.title = "wnk player";
    // Images only for seek previews (one sheet at a time): small budgets. The shell's HTTP (with its
    // disk cache) loads them; media keeps its own transport.
    config.http = true;
    config.image_memory_bytes = 8u << 20;
    config.texture_bytes = 8u << 20;
    config.script_env_prefix = "WNK_PLAYER";
    config.metrics_toggle = Action::Blue;
    return wnk::app::run(argc, argv, config, [] { return std::make_unique<Player>(); });
}
