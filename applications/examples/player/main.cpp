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
// Player keys: OK or Play/Pause toggles pause, Left/Right seek 10 s, Up opens the audio and subtitle
// track menu, Down cycles the subtitle tracks (a track the file marks as default starts on), Stop or
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
//   {"audio": "5:2,9:1"}                   scripted audio track changes (seconds after the player opens:
//                                          stream index), for checking the switch without a remote
//   {"ptsUnit": "ms"}                      feed NDL millisecond PTS instead of microseconds (experiment)
//   {"backend": "ndl"}                     force NDL (default: Starfish when the codecs suit it, else NDL)
//   {"backend": "starfish"}                force LG's Starfish pipeline
//   {"ndlReload": 1}                       pause and seek by reloading NDL (black picture; comparison)
//   {"leadMs": 400}                        feed at most this far ahead of the clock (default 1000)
//   {"ac3": false}                         never re-encode audio to AC-3 (audio Starfish cannot take
//                                          then sends the file to NDL, as before)
//   {"autoplay": 0}                        open that bundled clip at start (0..5), or stream (6..16)
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
    int lead_ms = 1000;
    bool ac3 = true;  // re-encode audio Starfish cannot take to AC-3
    std::string language;  // preferred audio language for DASH/HLS
    std::vector<std::pair<double, double>> seeks;  // scripted: (seconds after opening, relative seek)
    std::vector<std::pair<double, double>> audio_switches;  // scripted: (seconds after opening, audio track)
    DeviceMediaCaps device;
    std::shared_ptr<wnk::net::HttpTransport> http;  // null if libcurl is unavailable
    ContentKeys keys;         // ClearKey content keys ({"keys": ...})
    std::string license_url;  // ClearKey licence server ({"licenseUrl": ...}); else the manifest's
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
        o.keys = s.keys;
        o.license_url = s.license_url;
        return std::make_unique<AdaptiveSource>(o);
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
        : Screen(title), ctx_(ctx), uri_(std::move(uri)), seeks_(s.seeks), audio_switches_(s.audio_switches) {
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
            controller_->seek_relative(static_cast<int64_t>(seeks_[next_seek_].second * 1e6));
            ++next_seek_;
        }
        while (next_audio_ < audio_switches_.size() && ui.now_ms() - opened_ms_ >= audio_switches_[next_audio_].first * 1000) {
            const int track = static_cast<int>(audio_switches_[next_audio_].second);
            WNK_LOGI(kCat, "scripted audio track %d", track);
            controller_->set_audio_track(track);
            ++next_audio_;
        }
        const PlaybackSnapshot s = controller_->snapshot();
        const bool busy = s.state == PlayState::Opening || s.state == PlayState::Seeking;
        spinner_->set_visible(busy);
        status_->set_text(s.state == PlayState::Error ? "Error" : play_state_name(s.state));
        // A stream without a duration (live DASH/HLS) shows LIVE instead.
        const bool live = s.duration_us == kNoTime && (s.info.container == "dash" || s.info.container == "hls");
        time_->set_text(clock_text(s.position_us) + " / " + (live ? std::string("LIVE") : clock_text(s.duration_us)) +
                        (s.position_estimated ? "" : " ·"));
        if (s.duration_us > 0 && s.position_us != kNoTime) progress_->set_value(static_cast<float>(s.position_us) / s.duration_us);
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
        auto d = std::make_unique<Dialog>("Audio and subtitles");
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
        d->add_button("Close").name = "tracks.close";
        ui.show_dialog(std::move(d));
        if (current != nullptr) ui.focus(current);
        View *f = ui.focused();
        WNK_LOGI(kCat, "tracks menu open, focus=%s", f != nullptr ? f->name.c_str() : "<none>");
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
        for (const TrackInfo &t : tracks_) {
            if (t.index != track) continue;
            label = "Subtitles: " + track_name(t);
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
    std::string backend_name_;
    std::unique_ptr<PlaybackController> controller_;
    Column *bar_;
    Label *title_, *status_, *time_, *detail_;
    ProgressBar *progress_;
    Spinner *spinner_;
    bool opened_ = false;
    double last_input_ms_ = 0;
    double last_log_ms_ = 0;
    double opened_ms_ = -1;
    std::vector<std::pair<double, double>> seeks_;
    size_t next_seek_ = 0;
    std::vector<std::pair<double, double>> audio_switches_;
    size_t next_audio_ = 0;
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
            auto &b = list.add<Button>(st.title + "   —   " + st.detail, [this, st](Ui &ui) { play(ui, st.url, st.title, st.keys); });
            b.set_align(Align::Start, Align::Start);
            b.name = "stream" + std::to_string(i);
        }
        std::string info = std::string("Decoder: ") + (wnk::platform::kIsWebOS ? "NDL DirectMedia v2" : "none on the host (pacing backend, no picture)");
        info += "   ·   PTS unit: ";
        info += settings_.pts_unit == PtsUnit::Milliseconds ? "ms" : "µs";
        info += "   ·   " + FfmpegSource::version();
        col.add<Label>(info, TextRole::Small).set_muted(true).set_padding({0, 30, 0, 0});
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
            {"Audio and subtitle tracks", "MKV · H.264 · AAC English (1 kHz) and French (500 Hz) · SubRip English and French · Up",
             "h264-aac-subs.mkv"},
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
            home_->play(ui, st.url, st.title, st.keys);
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
