// SPDX-License-Identifier: GPL-3.0-or-later
//
// Example B: a TV-style app built from the kit's UI toolkit (wnk/ui) on the app shell (wnk/app).
//
//   ┌────────┬──────────────────────────────────────────────┐
//   │ Home   │  Continue watching  [card][card][card]...    │  horizontal rails (virtualised),
//   │ Browse │  Popular            [card][card][card]...    │  each remembering its position
//   │ Widgets│  New                [card][card][card]...    │
//   └────────┴──────────────────────────────────────────────┘
//
//   * the sidebar switches sections as it gains focus; Right enters the section, Left returns;
//   * Browse is a virtualised 1000-item poster grid (only the visible rows exist);
//   * OK on a card pushes a details screen (navigation stack); Back pops it and focus returns to
//     the card it came from;
//   * Widgets shows buttons, a modal dialog, a toast, a spinner and progress bars;
//   * Back at the root shows "Press Back again to exit"; Exit quits (both handled by the shell);
//   * Magic Remote pointer: hovering focuses, clicking is OK, the wheel scrolls the page or grid;
//   * leaving the app (Home, another app) suspends drawing; returning resumes it;
//   * the Blue key toggles the metrics overlay (frame timing, draw stats, memory).
//
// Scripted runs (host diagnostics; SAM launches cannot pass environment variables):
//   WNK_TVUI_KEYS="Left,Down,Right"  key names, move@X:Y, click@X:Y, wheel@N, bg, fg (see wnk/app)
//   WNK_TVUI_SHOT=/path/out.bmp      save the last frame, then quit
#include "wnk/app/app.hpp"
#include "wnk/audio/audio.hpp"
#include "wnk/log.hpp"
#include "wnk/net/lan.hpp"
#include "wnk/ui/collection.hpp"
#include "wnk/ui/forms.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/ui.hpp"
#include "wnk/ui/widgets.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace wnk::ui;
using wnk::input::Action;

constexpr const char *kFallbackAppId = "org.wnk.examples.tvui";
constexpr const char *kCat = "tvui";
constexpr int kPosters = 12;
const wnk::Size kCardCell{320, 236};  // 16:9 thumbnails (480x270) plus a title line
constexpr float kThumbAspect = 9.0f / 16.0f;

struct Catalogue {
    std::string poster_base;  // file:///.../posters
    std::string poster(size_t i) const {
        char name[32];
        std::snprintf(name, sizeof name, "/poster-%02zu.png", i % kPosters + 1);
        return poster_base + name;
    }
    static std::string title(size_t i) {
        static const char *kWords[] = {"Northern", "Lights", "Harbour", "Signal", "Quiet", "Summit", "Echo",
                                       "Atlas", "Ember", "Tide", "Orbit", "Meadow", "Glass", "River"};
        return std::string(kWords[i % 14]) + " " + kWords[(i * 7 + 3) % 14];
    }
};

// Key presses injected through SDL's event queue, so they take the same path as real keys.

// ---- screens ---------------------------------------------------------------------------------------

class DetailsScreen : public Screen {
 public:
    DetailsScreen(const Catalogue &cat, size_t item) : Screen(Catalogue::title(item)) {
        name = "details";
        set_padding({120, 100, 120, 80});
        auto &row = add<Row>(80);
        auto &poster = row.add<ImageView>(cat.poster(item));
        poster.set_size(640, 360).set_align(Align::Start, Align::Start);
        poster.set_background(std::nullopt, 16);
        auto &scroll = row.add<ScrollView>();
        scroll.set_width(Dim::fill());
        auto &col = scroll.add<Column>(28);
        col.add<Label>(title(), TextRole::Title);
        char meta[96];
        std::snprintf(meta, sizeof meta, "%zu  ·  %zu min  ·  Item %zu of 1000", 1990 + item % 35, 85 + item % 60, item + 1);
        col.add<Label>(meta).set_muted(true);
        col.add<Label>("A placeholder synopsis long enough to wrap over several lines, so the details page shows "
                       "word wrapping and the ellipsis on the last line. The kit's Label breaks on spaces, splits "
                       "words that are wider than a line at code point boundaries and shortens whatever does not fit "
                       "with an ellipsis, as this paragraph does when it runs past four lines.")
            .set_max_lines(4);
        auto &buttons = col.add<Row>(24);
        buttons.set_padding({0, 20, 0, 0});
        auto &play = buttons.add<Button>("Play", [t = title()](Ui &ui) { ui.toast("Playing " + t + " (no media yet)"); });
        play.name = "details.play";
        auto &fav = buttons.add<Button>("Add to list");
        fav.name = "details.fav";
        fav.set_on_click([&fav](Ui &ui) {
            const bool added = fav.text() == "Add to list";
            fav.set_text(added ? "In your list" : "Add to list");
            ui.toast(added ? "Added to your list" : "Removed from your list");
        });
        buttons.add<Button>("Back", [](Ui &ui) {
            // Same as the remote's Back key: the navigator pops this screen.
            wnk::input::ActionEvent back;
            back.action = Action::Back;
            ui.handle(back);
        }).name = "details.back";
        col.add<Label>("Cast & crew", TextRole::Body).set_bold(true).set_padding({0, 30, 0, 0});
        auto &cast = col.add<Grid>(3, 20);
        for (int i = 0; i < 9; ++i) {
            auto &b = cast.add<Button>(Catalogue::title(item + 17 * (i + 1)), [](Ui &) {});
            b.name = "details.cast" + std::to_string(i);
        }
    }
};

// Sidebar entry: with the D-pad the section follows focus; with the pointer it changes on click, so
// sweeping the pointer across the sidebar does not flip through every section (seen on the TV).
class NavItem : public Button {
 public:
    NavItem(std::string text, std::function<void(Ui &)> on_select) : Button(std::move(text)), on_select_(on_select) {
        set_align(Align::Stretch, Align::Start);
        set_on_click(std::move(on_select));
    }
    void on_focus_changed(Ui &ui, bool focused) override {
        if (focused && on_select_ && ui.focus_source() != FocusSource::Pointer) on_select_(ui);
    }
    bool selected = false;  // the section shown on the right, marked even when focus is elsewhere

 protected:
    void draw(Ui &ui) override {
        Button::draw(ui);
        if (!selected) return;
        const wnk::Rect f = frame();
        ui.renderer().fill_rect({f.x + 14, f.y + f.h * 0.3f, 6, f.h * 0.4f}, ui.fade(ui.theme().accent), 3);
    }

 private:
    std::function<void(Ui &)> on_select_;
};

// Interface sounds, generated rather than bundled: a short tick when focus moves and a chord on demand.
struct Sounds {
    wnk::audio::Mixer *mixer = nullptr;  // nullptr without an audio device
    bool on = false;
    wnk::audio::Sound tick = wnk::audio::tone(1760, 0.025f, 0.12f);
    wnk::audio::Sound chord = [] {
        auto pcm = std::make_shared<wnk::audio::Pcm>();
        for (float hz : {523.25f, 659.25f, 783.99f}) {
            auto t = wnk::audio::tone(hz, 0.6f, 0.15f);
            pcm->samples.resize(t->samples.size());
            for (size_t i = 0; i < t->samples.size(); ++i) pcm->samples[i] += t->samples[i];
        }
        return wnk::audio::Sound(pcm);
    }();
    int played = 0;
    void play(const wnk::audio::Sound &s) {
        if (mixer != nullptr && mixer->play(s) != wnk::audio::kNoVoice) ++played;
    }
};

class HomeScreen : public Screen {
 public:
    HomeScreen(wnk::app::Context &ctx, const Catalogue &cat, Navigator &nav, Sounds &sounds)
        : Screen("home"), ctx_(ctx), cat_(cat), nav_(nav), sounds_(sounds) {
        name = "home";
        auto &row = add<Row>(0);

        auto &sidebar = row.add<Column>(16);
        sidebar.set_width(Dim::fixed(320));
        sidebar.set_padding({40, 80, 40, 40});
        sidebar.set_focus_scope(false, true);
        sidebar.set_background(wnk::render::Color::hex(0x0A0F18FF));
        sidebar.add<Label>("WNK TV", TextRole::Title).set_padding({8, 0, 0, 40});

        content_ = &row.add<View>();
        content_->set_width(Dim::fill());
        content_->set_padding({60, 60, 0, 0});

        const char *names[] = {"Home", "Browse", "Widgets", "Settings"};
        View *sections[] = {&build_home(), &build_browse(), &build_widgets(), &build_settings()};
        for (int i = 0; i < 4; ++i) {
            sections_.push_back(sections[i]);
            auto &item = sidebar.add<NavItem>(names[i], [this, i](Ui &) { show_section(i); });
            item.name = std::string("nav.") + names[i];
            nav_items_.push_back(&item);
        }
        show_section(0);
    }

    View &first_rail() { return *rails_[0]; }

    void show_network(const wnk::platform::NetworkStatus &s) {
        if (network_row_ != nullptr) network_row_->set_value(s.summary());
    }

    std::string describe() const { return std::to_string(grid_->live_count()); }

 private:
    void show_section(int i) {
        if (i == current_) return;
        current_ = i;
        for (size_t s = 0; s < sections_.size(); ++s) {
            sections_[s]->set_visible(static_cast<int>(s) == i);
            if (s < nav_items_.size()) nav_items_[s]->selected = static_cast<int>(s) == i;
        }
        WNK_LOGI(kCat, "section %d", i);
    }

    void open_details(Ui &ui, size_t item) {
        WNK_LOGI(kCat, "open details for item %zu", item);
        nav_.push(ui, std::make_unique<DetailsScreen>(cat_, item));
    }

    View &build_home() {
        // The rails are taller than the screen: the scroll view follows focus down and back up.
        auto &scroll = content_->add<ScrollView>();
        scroll.name = "section.home";
        scroll.margin = 120;
        auto &col = scroll.add<Column>(24);
        col.set_padding({0, 0, 0, 60});
        const char *titles[] = {"Continue watching", "Popular", "New this week"};
        for (int r = 0; r < 3; ++r) {
            col.add<Label>(titles[r]).set_bold(true).set_padding({16, 0, 0, 0});
            auto &rail = col.add<CollectionView>(Axis::Horizontal, 1, kCardCell, 28);
            rail.set_width(Dim::fill());
            rail.name = "rail" + std::to_string(r);
            const size_t offset = static_cast<size_t>(r) * 40;
            rail.set_items(40, [this, r, offset](Ui &, size_t i) {
                auto card = std::make_unique<Card>(Catalogue::title(offset + i), cat_.poster(offset + i));
                card->image_aspect = kThumbAspect;
                card->name = "rail" + std::to_string(r) + ".item" + std::to_string(i);
                return card;
            });
            rail.on_activate = [this, offset](Ui &ui, size_t i) { open_details(ui, offset + i); };
            rails_.push_back(&rail);
        }
        return scroll;
    }

    View &build_browse() {
        auto &col = content_->add<Column>(20);
        col.name = "section.browse";
        col.add<Label>("Browse all", TextRole::Title);
        auto &grid = col.add<CollectionView>(Axis::Vertical, 4, wnk::Size{kCardCell.w, kCardCell.h + 26}, 28);
        grid.set_height(Dim::fill());
        grid.name = "grid";
        grid.set_items(1000, [this](Ui &, size_t i) {
            auto card = std::make_unique<Card>(Catalogue::title(i), cat_.poster(i), "Item " + std::to_string(i + 1));
            card->image_aspect = kThumbAspect;
            card->name = "grid.item" + std::to_string(i);
            return card;
        });
        grid.on_activate = [this](Ui &ui, size_t i) { open_details(ui, i); };
        grid_ = &grid;
        return col;
    }

    View &build_widgets() {
        auto &col = content_->add<Column>(36);
        col.name = "section.widgets";
        col.set_padding({16, 0, 60, 0});
        col.add<Label>("Widgets", TextRole::Title);
        // Outside the app: the TV's browser, its notification cards, and the home network.
        auto &system = col.add<Row>(24);
        system.add<Button>("Open a web page", [this](Ui &) { ctx_.open_url("https://www.webosbrew.org/"); }).name = "widgets.url";
        system.add<Button>("TV notification", [this](Ui &ui) {
            ctx_.system_toast("Hello from WNK TV", [&ui](bool ok) { ui.toast(ok ? "Sent to the TV" : "The TV refused the notification"); });
        }).name = "widgets.notify";
        system.add<Button>("Find devices", [this](Ui &ui) { find_devices(ui); }).name = "widgets.devices";
        auto &buttons = col.add<Row>(24);
        buttons.add<Button>("Show a dialog", [](Ui &ui) {
            auto d = std::make_unique<Dialog>("Remove from list?",
                                              "Focus is trapped in this dialog. Back or Cancel closes it and focus "
                                              "returns to the button that opened it.");
            d->add_button("Cancel").name = "dialog.cancel";
            d->add_button("Remove", [](Ui &u) { u.toast("Removed"); }).name = "dialog.remove";
            d->on_dismiss = [](Ui &u) {
                View *f = u.focused();
                WNK_LOGI(kCat, "dialog closed focus=%s", f != nullptr ? f->name.c_str() : "<none>");
            };
            ui.show_dialog(std::move(d));
            WNK_LOGI(kCat, "dialog opened");
        }).name = "widgets.dialog";
        buttons.add<Button>("Show a toast", [](Ui &ui) { ui.toast("Toasts queue and fade out on their own"); }).name =
            "widgets.toast";
        buttons.add<Button>("Play a sound", [this](Ui &ui) {
            if (sounds_.mixer == nullptr) {
                ui.toast("No audio output on this device");
                return;
            }
            sounds_.play(sounds_.chord);
            WNK_LOGI(kCat, "sound: chord");
        }).name = "widgets.sound";
        auto &disabled = buttons.add<Button>("Disabled");
        disabled.set_enabled(false);
        progress_ = &col.add<ProgressBar>();
        progress_->set_width(Dim::fixed(900));
        progress_->set_align(Align::Start, Align::Start);
        buttons.add<Button>("Advance progress", [this](Ui &) {
            const float v = progress_->value() + 0.2f;
            progress_->set_value(v > 1.01f ? 0.0f : v);
        }).name = "widgets.progress";
        auto &busy = col.add<Row>(30);
        busy.add<Spinner>(72);
        auto &ind = busy.add<ProgressBar>();
        ind.set_indeterminate(true);
        ind.set_width(Dim::fixed(600));
        ind.set_align(Align::Start, Align::Center);
        col.add<Label>("Long labels are shortened with an ellipsis when they do not fit in the space they are given, "
                       "which this one demonstrates on a single line.")
            .set_muted(true);
        return col;
    }

    // DNS-SD: which kinds of service answer on this network (Cast, AirPlay, printers...), on a worker.
    void find_devices(Ui &ui) {
        ui.toast("Looking for devices…");
        wnk::async::run_then_post<std::vector<std::string>>(
            ctx_.pool(), ctx_.dispatcher(), life_.weak(), {},
            [](const wnk::async::CancellationToken &t) {
                std::vector<std::string> kinds;
                std::string err;
                for (const auto &s : wnk::net::mdns_browse("_services._dns-sd._udp.local", std::chrono::milliseconds(1500), t, &err)) {
                    kinds.push_back(s.name);
                }
                if (!err.empty()) WNK_LOGW(kCat, "find devices: %s", err.c_str());
                return kinds;
            },
            [this](std::vector<std::string> kinds) {
                WNK_LOGI(kCat, "find devices: %zu service types", kinds.size());
                std::string list;
                for (const auto &k : kinds) list += (list.empty() ? "" : ", ") + k;
                auto d = std::make_unique<Dialog>("On this network", kinds.empty() ? "Nothing answered." : list);
                d->add_button("Close");
                ctx_.ui().show_dialog(std::move(d));
            });
    }

    // A settings screen from the form controls: tabs over three pages of rows, a value row that edits
    // text in a dialog, and a QR code for continuing on a phone.
    View &build_settings() {
        auto &col = content_->add<Column>(28);
        col.name = "section.settings";
        col.set_padding({16, 0, 60, 0});
        col.add<Label>("Settings", TextRole::Title);
        auto &tabs = col.add<Tabs>(std::vector<std::string>{"General", "Playback", "Pair"}, 0, [this](Ui &, int i) {
            for (size_t p = 0; p < settings_pages_.size(); ++p) settings_pages_[p]->set_visible(static_cast<int>(p) == i);
            WNK_LOGI(kCat, "settings tab %d", i);
        });
        tabs.name = "settings.tabs";
        for (size_t i = 0; i < tabs.count(); ++i) tabs.tab(i).name = "settings.tab" + std::to_string(i);
        auto &stack = col.add<View>();
        stack.set_height(Dim::fill());

        auto &general = stack.add<Column>(16);
        general.set_width(Dim::fixed(1100)).set_align(Align::Start, Align::Start);
        auto log_bool = [](const char *what) { return [what](Ui &, bool on) { WNK_LOGI(kCat, "setting %s = %s", what, on ? "on" : "off"); }; };
        general.add<Toggle>("Play the next episode automatically", true, log_bool("autoplay")).name = "settings.autoplay";
        auto &check = general.add<Toggle>("Show clock", false, log_bool("clock"));
        check.set_style(Toggle::Style::Check);
        check.name = "settings.clock";
        auto &size = general.add<Slider>("Subtitle size", 50, 200, 10, 100,
                                         [](Ui &, float v) { WNK_LOGI(kCat, "setting subtitle size = %.0f", static_cast<double>(v)); });
        size.set_format([](float v) { return std::to_string(static_cast<int>(v)) + " %"; });
        size.name = "settings.subsize";
        auto &device = general.add<ValueRow>("Device name", "Living room TV");
        device.set_description("Shown to phones that cast to this TV");
        device.name = "settings.device";
        device.set_on_click([&device](Ui &ui) {
            auto d = std::make_unique<Dialog>("Device name");
            auto &field = d->body().add<TextField>("Name");
            field.set_text(device.value()).set_max_length(32).set_width(Dim::fill());
            field.name = "settings.device.field";
            Dialog *dialog = d.get();
            field.set_on_submit([&device, dialog](Ui &u, const std::string &text) {
                if (!text.empty()) device.set_value(text);
                WNK_LOGI(kCat, "setting device name = %s", device.value().c_str());
                dialog->dismiss(u);
            });
            d->add_button("Cancel");
            ui.show_dialog(std::move(d));
            ui.focus(&field);
        });

        general.add<Toggle>("Interface sounds", sounds_.on, [this](Ui &, bool on) {
            sounds_.on = on;
            WNK_LOGI(kCat, "setting interface sounds = %s", on ? "on" : "off");
        }).name = "settings.sounds";
        network_row_ = &general.add<ValueRow>("Network", ctx_.network().summary());
        network_row_->name = "settings.network";

        auto &playback = stack.add<Column>(16);
        playback.set_width(Dim::fixed(1100)).set_align(Align::Start, Align::Start);
        playback.add<Choice>("Quality", std::vector<std::string>{"Automatic", "2160p", "1080p", "720p", "480p"}, 0,
                             [](Ui &, int i) { WNK_LOGI(kCat, "setting quality = %d", i); })
            .name = "settings.quality";
        playback.add<Choice>("Audio language", std::vector<std::string>{"Original", "English", "Français", "Deutsch", "日本語"}, 0,
                             [](Ui &, int i) { WNK_LOGI(kCat, "setting audio language = %d", i); })
            .name = "settings.audio";

        auto &pair = stack.add<Row>(48);
        pair.set_align(Align::Start, Align::Start);
        auto &text_col = pair.add<Column>(16);
        text_col.set_width(Dim::fixed(760));
        text_col.add<Label>("Continue on your phone", TextRole::Title);
        text_col.add<Label>("Scan the code, or open example.org/pair and enter WNK-4821.").set_max_lines(2);
        auto &terms = text_col.add<TextView>(
            "Pairing links this TV to your account so it can play what you choose on your phone. The TV keeps a token "
            "for the account, not your password, and you can remove the link from the account page at any time. "
            "Nothing you watch is shared with the phone beyond what you send to the TV yourself. The code above is "
            "valid for ten minutes; a new one appears when it expires. This text is long on purpose: it shows a "
            "TextView, which scrolls with Up and Down when it is focused and passes focus on at either end, so a "
            "long licence or privacy notice can sit in a settings page without a page of its own. A thin bar on the "
            "right shows where in the text the frame is.",
            TextRole::Small);
        terms.set_height(Dim::fixed(130));
        terms.name = "settings.terms";
        auto &qr = pair.add<QrCode>("https://example.org/pair?code=WNK-4821");
        qr.set_size(320, 320);

        settings_pages_ = {&general, &playback, &pair};
        playback.set_visible(false);
        pair.set_visible(false);
        return col;
    }

    wnk::app::Context &ctx_;
    const Catalogue &cat_;
    Navigator &nav_;
    Sounds &sounds_;
    ValueRow *network_row_ = nullptr;
    wnk::async::Lifetime life_;
    std::vector<View *> settings_pages_;
    View *content_ = nullptr;
    std::vector<View *> sections_;
    std::vector<NavItem *> nav_items_;
    std::vector<CollectionView *> rails_;
    CollectionView *grid_ = nullptr;
    ProgressBar *progress_ = nullptr;
    int current_ = -1;
};

// ---- app -------------------------------------------------------------------------------------------

class TvUi : public wnk::app::Delegate {
 public:
    void on_start(wnk::app::Context &ctx) override {
        cat_.poster_base = "file://" + ctx.launch().app_dir + "/posters";
        Ui &ui = ctx.ui();
        nav_ = &ui.root().add<Navigator>();
        nav_->on_change = [this](Ui &u, Screen &s) {
            View *f = u.focused();
            const char *focus = f != nullptr ? f->name.c_str() : "<pending>";
            WNK_LOGI(kCat, "screen %s depth %zu focus=%s", s.title().c_str(), nav_->depth(), focus);
            if (nav_->depth() < depth_) WNK_LOGI(kCat, "back to %s focus=%s", s.title().c_str(), focus);
            depth_ = nav_->depth();
        };
        sounds_.mixer = ctx.audio();
        const auto params = nlohmann::json::parse(ctx.launch().params_json.empty() ? "{}" : ctx.launch().params_json, nullptr, false);
        sounds_.on = params.is_object() && params.value("sounds", false);
        if (sounds_.on) WNK_LOGI(kCat, "interface sounds on");
        home_ = &static_cast<HomeScreen &>(nav_->push(ui, std::make_unique<HomeScreen>(ctx, cat_, *nav_, sounds_)));
        ui.focus_within(&home_->first_rail());
    }

    // Raw pad input before the UI sees it: X has no UI meaning, so this app takes it (a game would
    // take everything here). Everything else navigates as usual.
    bool on_gamepad(wnk::app::Context &, const wnk::input::GamepadEvent &e) override {
        if (e.type != wnk::input::GamepadEvent::Type::Button || e.button != wnk::input::PadButton::X) return false;
        if (e.down) {
            WNK_LOGI(kCat, "gamepad %d: X pressed (handled by the app)", e.id);
            sounds_.play(sounds_.chord);
        }
        return true;
    }

    void on_network(wnk::app::Context &, const wnk::platform::NetworkStatus &s) override {
        if (home_ != nullptr) home_->show_network(s);
    }

    void on_frame(wnk::app::Context &ctx, float) override {
        View *f = ctx.ui().focused();
        if (f != last_focus_ && last_focus_ != nullptr && sounds_.on) sounds_.play(sounds_.tick);
        last_focus_ = f;
    }

    void on_stop(wnk::app::Context &ctx) override {
        const auto s = ctx.ui().stats();
        WNK_LOGI(kCat, "ui stats: views %zu, focus nodes %zu, layouts %llu, browse grid cells %s", s.views,
                 s.focus_nodes, static_cast<unsigned long long>(s.layouts), home_->describe().c_str());
        WNK_LOGI(kCat, "sounds played: %d", sounds_.played);
    }

 private:
    Catalogue cat_;
    Sounds sounds_;
    View *last_focus_ = nullptr;
    Navigator *nav_ = nullptr;
    HomeScreen *home_ = nullptr;
    size_t depth_ = 0;
};

}  // namespace

int main(int argc, char **argv) {
    wnk::app::AppConfig config;
    config.fallback_app_id = kFallbackAppId;
    config.title = "wnk tvui";
    config.script_env_prefix = "WNK_TVUI";
    config.audio = true;  // interface sounds (Settings) and the Widgets chord
    config.metrics_toggle = wnk::input::Action::Blue;  // the blue colour key shows frame timing on screen
    return wnk::app::run(argc, argv, config, [] { return std::make_unique<TvUi>(); });
}
