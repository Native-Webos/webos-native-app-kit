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
// Player keys: OK or Play/Pause toggles pause, Left/Right seek 10 s, Down cycles the subtitle tracks
// (a track the file marks as default starts on), Stop or Back leaves. Any key
// shows the controls; they hide after 4 s of playback. Pause and seek keep the decoder loaded, so the
// picture holds; backgrounding the app pauses playback and releases the decoder, and playback resumes
// on return if it was playing.
//
// Launch params (ares-launch --params):
//   {"uri": "file:///path/to/file.mkv"}   play that file straight away; http(s):// URIs stream through
//                                          the kit's HTTP client (byte ranges make them seekable); .mpd
//                                          and .m3u8 URIs play as DASH/HLS (AdaptiveSource)
//   {"lang": "de"}                         preferred audio language for DASH/HLS
//   {"ptsUnit": "ms"}                      feed NDL millisecond PTS instead of microseconds (experiment)
//   {"backend": "ndl"}                     force NDL (default: Starfish when the codecs suit it, else NDL)
//   {"backend": "starfish"}                force LG's Starfish pipeline
//   {"ndlReload": 1}                       pause and seek by reloading NDL (black picture; comparison)
//   {"leadMs": 400}                        feed at most this far ahead of the clock (default 1000)
//   {"ac3": false}                         never re-encode audio to AC-3 (audio Starfish cannot take
//                                          then sends the file to NDL, as before)
//   {"autoplay": 0}                        open that bundled clip at start (0..5), or stream (6..9)
#include "wnk/app/app.hpp"
#include "wnk/log.hpp"
#include "wnk/media/auto_backend.hpp"
#include "wnk/media/controller.hpp"
#include "wnk/media/device_caps.hpp"
#include "wnk/media/adaptive_source.hpp"
#include "wnk/media/ffmpeg_source.hpp"
#include "wnk/media/ndl_backend.hpp"
#include "wnk/media/pacing_backend.hpp"
#include "wnk/media/starfish_backend.hpp"
#include "wnk/media/subtitle_view.hpp"
#include "wnk/platform/process.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/widgets.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

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
    std::string title, detail, url;
};

struct Settings {
    std::string clips_dir;
    PtsUnit pts_unit = PtsUnit::Microseconds;
    bool ndl_reload = false;
    std::string backend = "auto";  // auto (Starfish when the codecs suit it, else NDL), starfish, ndl
    int lead_ms = 1000;
    bool ac3 = true;  // re-encode audio Starfish cannot take to AC-3
    std::string language;  // preferred audio language for DASH/HLS
    DeviceMediaCaps device;
    std::shared_ptr<wnk::net::HttpTransport> http;  // null if libcurl is unavailable
};

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
    if (starfish && ndl) return std::make_unique<AutoBackend>(std::make_unique<StarfishBackend>(so), make_ndl());
    if (starfish) return std::make_unique<StarfishBackend>(so);
    if (ndl) return make_ndl();
    WNK_LOGW(kCat, "no media backend (Starfish: %s; NDL: %s): no picture", why_sf.empty() ? "not wanted" : why_sf.c_str(),
             why_ndl.empty() ? "not wanted" : why_ndl.c_str());
    return std::make_unique<PacingBackend>();
}

// DASH/HLS manifests go to the AdaptiveSource, everything else to FfmpegSource. Renditions are
// chosen from what the TV reports: HEVC first when it has HEVC, 2160p only on a UHD panel.
std::unique_ptr<MediaSource> make_source(const Settings &s, const std::string &uri) {
    if (AdaptiveSource::handles(uri)) {
        AdaptiveSourceOptions o;
        o.http = s.http;
        o.ac3_encode = s.ac3;
        o.language = s.language;
        if (s.device.hevc == false) o.video_codecs = {CodecId::H264};
        if (s.device.uhd_panel != true) o.max_height = 1080;
        o.dolby_vision = s.device.dolby_vision == true;
        return std::make_unique<AdaptiveSource>(o);
    }
    return std::make_unique<FfmpegSource>(FfmpegSourceOptions{s.http, {}, s.ac3});
}

// ---- player screen ---------------------------------------------------------------------------------

class PlayerScreen : public Screen {
 public:
    PlayerScreen(wnk::app::Context &ctx, const Settings &s, std::string uri, std::string title)
        : Screen(title), ctx_(ctx), uri_(std::move(uri)) {
        name = "player";
        // Under the controls: subtitles over the whole picture.
        subs_view_ = &add<SubtitleView>();
        subs_view_->set_width(Dim::fill()).set_height(Dim::fill());
        subs_view_->set_provider([this] { return controller_ ? controller_->subtitles_now() : SubtitleView::Cues{}; });
        bar_ = &add<Column>(14);
        bar_->set_align(Align::Stretch, Align::End);
        bar_->set_padding({80, 40, 80, 60});
        bar_->set_background(wnk::render::Color::hex(0x000000B0));
        auto &top = bar_->add<Row>(24);
        title_ = &top.add<Label>(title, TextRole::Title);
        title_->set_width(Dim::fill());
        status_ = &top.add<Label>("Opening…");
        status_->set_muted(true).set_align(Align::Start, Align::End);
        progress_ = &bar_->add<ProgressBar>();
        auto &bottom = bar_->add<Row>(24);
        time_ = &bottom.add<Label>("--:-- / --:--");
        time_->set_width(Dim::fill());
        subs_label_ = &bottom.add<Label>("", TextRole::Small);
        bottom.add<Label>("OK pause  ·  Left/Right 10 s  ·  Down subtitles  ·  Back leaves", TextRole::Small).set_muted(true);
        detail_ = &bar_->add<Label>("", TextRole::Small);
        detail_->set_muted(true);
        spinner_ = &add<Spinner>(96);

        auto backend = make_backend(s);
        backend_name_ = backend->name();
        PlaybackOptions po;
        po.max_lead_us = static_cast<int64_t>(s.lead_ms) * 1000;
        controller_ = std::make_unique<PlaybackController>(make_source(s, uri_), std::move(backend), ctx.dispatcher(), po);
        controller_->set_state_callback([this](PlayState from, PlayState to, const PlaybackSnapshot &snap) {
            WNK_LOGI(kCat, "player state %s -> %s%s%s", play_state_name(from), play_state_name(to),
                     snap.error.empty() ? "" : ": ", snap.error.c_str());
            if (to == PlayState::Ready) {
                controller_->set_video_area({0, 0, 1920, 1080});  // again, now the stream size is known
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
            case Action::Rewind: controller_->seek_relative(-10000000); return true;
            case Action::Right:
            case Action::FastForward: controller_->seek_relative(10000000); return true;
            case Action::Stop: {
                wnk::input::ActionEvent back;
                back.action = Action::Back;
                return ui.handle(back);
            }
            case Action::Down:
                if (e.phase == Phase::Press) next_subtitles();
                return true;
            case Action::Up: return true;  // nothing to move to; keep focus here
            default: return false;
        }
    }

    void update(Ui &ui, float) override {
        if (!controller_) return;
        const PlaybackSnapshot s = controller_->snapshot();
        const bool busy = s.state == PlayState::Opening || s.state == PlayState::Seeking;
        spinner_->set_visible(busy);
        status_->set_text(s.state == PlayState::Error ? "Error" : play_state_name(s.state));
        time_->set_text(clock_text(s.position_us) + " / " + clock_text(s.duration_us) + (s.position_estimated ? "" : " ·"));
        if (s.duration_us > 0 && s.position_us != kNoTime) progress_->set_value(static_cast<float>(s.position_us) / s.duration_us);
        if (s.state == PlayState::Error) detail_->set_text(s.error);
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
    }

 private:
    void show_controls() { last_input_ms_ = ctx_.ui().now_ms(); }

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
        for (const TrackInfo &t : tracks_) {
            if (t.index != track) continue;
            label = "Subtitles: " + (!t.title.empty() ? t.title : !t.language.empty() ? t.language : "track " + std::to_string(track));
            label += " (" + t.codec_label + ")";
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
        for (const auto &t : s.info.tracks) {
            if (t.type == TrackType::Audio && s.format.audio) {
                d += "  ·  " + t.codec_label + " " + std::to_string(t.channels) + " ch";
                if (s.format.audio->codec == CodecId::PcmS16) d += " decoded to PCM stereo";
                break;
            }
        }
        d += "  ·  " + std::string(controller_->backend_name());
        detail_->set_text(d);
    }

    wnk::app::Context &ctx_;
    std::string uri_;
    SubtitleView *subs_view_ = nullptr;
    Label *subs_label_ = nullptr;
    std::vector<TrackInfo> tracks_;
    std::string shown_;
    bool resume_on_return_ = false;  // playing when the app went to the background
    std::string backend_name_;
    std::unique_ptr<PlaybackController> controller_;
    Column *bar_;
    Label *title_, *status_, *time_, *detail_;
    ProgressBar *progress_;
    Spinner *spinner_;
    bool opened_ = false;
    double last_input_ms_ = 0;
    double last_log_ms_ = 0;
};

// ---- clip list -------------------------------------------------------------------------------------

class HomeScreen : public Screen {
 public:
    HomeScreen(wnk::app::Context &ctx, Navigator &nav, Settings settings, const std::vector<Clip> &clips,
               const std::vector<Stream> &streams)
        : Screen("home"), ctx_(ctx), nav_(nav), settings_(std::move(settings)) {
        name = "home";
        auto &col = add<Column>(20);
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
            auto &b = list.add<Button>(st.title + "   —   " + st.detail, [this, st](Ui &ui) { play(ui, st.url, st.title); });
            b.set_align(Align::Start, Align::Start);
            b.name = "stream" + std::to_string(i);
        }
        std::string info = std::string("Decoder: ") + (wnk::platform::kIsWebOS ? "NDL DirectMedia v2" : "none on the host (pacing backend, no picture)");
        info += "   ·   PTS unit: ";
        info += settings_.pts_unit == PtsUnit::Milliseconds ? "ms" : "µs";
        info += "   ·   " + FfmpegSource::version();
        col.add<Label>(info, TextRole::Small).set_muted(true).set_padding({0, 30, 0, 0});
    }

    void play(Ui &ui, const std::string &uri, const std::string &title) {
        WNK_LOGI(kCat, "play %s", uri.c_str());
        nav_.push(ui, std::make_unique<PlayerScreen>(ctx_, settings_, uri, title));
    }

 private:
    wnk::app::Context &ctx_;
    Navigator &nav_;
    Settings settings_;
};

class Player : public wnk::app::Delegate {
 public:
    void on_start(wnk::app::Context &ctx) override {
        Settings s;
        s.clips_dir = ctx.launch().app_dir + "/clips";
        std::string curl_error;
        s.http = wnk::net::make_curl_transport({}, &curl_error);
        if (!s.http) WNK_LOGW(kCat, "no HTTP streaming: %s", curl_error.c_str());
        std::string direct_uri;
        int autoplay = -1;
        const auto params = nlohmann::json::parse(ctx.launch().params_json.empty() ? "{}" : ctx.launch().params_json, nullptr, false);
        if (params.is_object()) {
            if (params.contains("ptsUnit") && params["ptsUnit"] == "ms") s.pts_unit = PtsUnit::Milliseconds;
            if (params.contains("ndlReload") && params["ndlReload"] == 1) s.ndl_reload = true;
            if (params.contains("backend") && params["backend"].is_string()) s.backend = params["backend"];
            if (params.contains("leadMs") && params["leadMs"].is_number_integer()) {
                s.lead_ms = std::clamp(params["leadMs"].get<int>(), 100, 5000);
            }
            if (params.contains("ac3") && params["ac3"].is_boolean()) s.ac3 = params["ac3"];
            if (params.contains("lang") && params["lang"].is_string()) s.language = params["lang"];
            if (params.contains("uri") && params["uri"].is_string()) direct_uri = params["uri"];
            if (params.contains("autoplay") && params["autoplay"].is_number_integer()) autoplay = params["autoplay"];
        }
        if (wnk::platform::kIsWebOS) {
            s.device = DeviceMediaCaps::detect();
            WNK_LOGI(kCat, "device media: %s", s.device.summary().c_str());
        }
        WNK_LOGI(kCat, "%s; PTS unit %s; NDL pause/seek by %s; lead %d ms", FfmpegSource::version().c_str(),
                 s.pts_unit == PtsUnit::Milliseconds ? "ms" : "us", s.ndl_reload ? "reload" : "hold/splice", s.lead_ms);
        const std::vector<Clip> clips = {
            {"H.264 + AAC", "MP4 · 1080p25 · AAC as is (Starfish) or decoded to PCM (NDL)", "h264-aac.mp4"},
            {"HEVC + AC-3 5.1", "MKV · 1080p25 · AC-3 as is (Starfish) or downmixed to PCM stereo (NDL)", "hevc-ac3-5.1.mkv"},
            {"H.264 + MP3", "MPEG-TS · 1080p25 · MP3 re-encoded to AC-3 (Starfish) or decoded to PCM (NDL)", "h264-mp3.ts"},
            {"Subtitles", "MKV · H.264 + AAC · SubRip English (default) and French · Down switches", "h264-aac-subs.mkv"},
            {"H.264 + FLAC 7.1", "MKV · 1080p25 · 96 kHz 7.1 FLAC re-encoded to AC-3 5.1 48 kHz (Starfish)", "h264-flac-7.1.mkv"},
            {"H.264 + Opus", "MKV · 1080p25 · Opus re-encoded to AC-3 (Starfish) or as is (NDL)", "h264-opus.mkv"},
        };
        const std::vector<Stream> streams = {
            {"Dolby Vision + Atmos", "DASH · Dolby test stream · HEVC up to 2160p59.94 · E-AC-3 JOC",
             "https://media.developer.dolby.com/DolbyVision_Atmos/profile8.1_DASH/p8.1.mpd"},
            {"Apple bipbop", "HLS · fMP4 byte ranges · H.264/HEVC · separate AAC, AC-3, E-AC-3",
             "https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_adv_example_hevc/master.m3u8"},
            {"Mux test stream", "HLS · MPEG-TS · H.264 + AAC muxed", "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"},
            {"Shaka angel-one", "DASH · SegmentBase (sidx) · H.264 · AAC in six languages",
             "https://storage.googleapis.com/shaka-demo-assets/angel-one/dash.mpd"},
        };
        Ui &ui = ctx.ui();
        nav_ = &ui.root().add<Navigator>();
        home_ = &static_cast<HomeScreen &>(nav_->push(ui, std::make_unique<HomeScreen>(ctx, *nav_, s, clips, streams)));
        const int n_clips = static_cast<int>(clips.size());
        if (!direct_uri.empty()) {
            home_->play(ui, direct_uri, direct_uri.substr(direct_uri.find_last_of('/') + 1));
        } else if (autoplay >= 0 && autoplay < n_clips) {
            home_->play(ui, "file://" + s.clips_dir + "/" + clips[autoplay].file, clips[autoplay].title);
        } else if (autoplay >= n_clips && autoplay < n_clips + static_cast<int>(streams.size())) {
            const Stream &st = streams[static_cast<size_t>(autoplay - n_clips)];
            home_->play(ui, st.url, st.title);
        }
    }

    void on_lifecycle(wnk::app::Context &, const wnk::app::Lifecycle::Transition &t) override {
        // Leaving the app: stop decoding, release NDL and tell it.
        if (t.to == wnk::app::State::Suspended) {
            if (auto *p = dynamic_cast<PlayerScreen *>(nav_->top())) p->pause_for_background();
            if (wnk::platform::kIsWebOS) NdlBackend::set_app_foreground(false);
        } else if (t.to == wnk::app::State::Resuming && wnk::platform::kIsWebOS) {
            NdlBackend::set_app_foreground(true);
        } else if (t.from == wnk::app::State::Resuming && t.to == wnk::app::State::Running) {
            if (auto *p = dynamic_cast<PlayerScreen *>(nav_->top())) p->return_from_background();
        }
    }

    void on_stop(wnk::app::Context &) override {
        if (auto *p = dynamic_cast<PlayerScreen *>(nav_ != nullptr ? nav_->top() : nullptr)) p->stop();
    }

 private:
    Navigator *nav_ = nullptr;
    HomeScreen *home_ = nullptr;
};

}  // namespace

int main(int argc, char **argv) {
    wnk::app::AppConfig config;
    config.fallback_app_id = "org.wnk.examples.player";
    config.title = "wnk player";
    config.images = false;
    config.script_env_prefix = "WNK_PLAYER";
    config.metrics_toggle = Action::Blue;
    return wnk::app::run(argc, argv, config, [] { return std::make_unique<Player>(); });
}
