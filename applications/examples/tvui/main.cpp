// SPDX-License-Identifier: GPL-3.0-or-later
//
// Example B: a TV-style app built only from the kit's UI toolkit (wnk/ui).
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
//   * Back at the root shows "Press Back again to exit"; Exit quits;
//   * Magic Remote pointer: hovering focuses, clicking is OK, the wheel scrolls the page or grid.
//
// Measurement log (for docs/compatibility/records.json): every raw key-down with its scancode,
// pointer buttons/wheel/visibility events, and frame timing every 300 frames.
//
// Scripted runs (host diagnostics; SAM launches cannot pass environment variables):
//   WNK_TVUI_KEYS="Left,Down,Right"  SDL scancode names injected as key presses, 6 frames apart;
//                                    also move@X:Y, click@X:Y (logical 1920x1080 units) and wheel@N
//   WNK_TVUI_SHOT=/path/out.bmp      save the last frame, then quit
#include "wnk/async/async.hpp"
#include "wnk/image/image.hpp"
#include "wnk/image/textures.hpp"
#include "wnk/input/input.hpp"
#include "wnk/input/sdl_input.hpp"
#include "wnk/log.hpp"
#include "wnk/platform/process.hpp"
#include "wnk/render/gles2_renderer.hpp"
#include "wnk/ui/collection.hpp"
#include "wnk/ui/image_source.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/ui.hpp"
#include "wnk/ui/widgets.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace wnk::ui;
using wnk::input::Action;

constexpr const char *kFallbackAppId = "org.wnk.examples.tvui";
constexpr const char *kCat = "tvui";
constexpr Uint32 kDoubleBackMs = 1500;
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
class Script {
 public:
    struct Step {
        enum Kind { Key, Move, Click, Wheel } kind = Key;
        SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
        float x = 0, y = 0;
    };
    Script() {
        if (const char *keys = SDL_getenv("WNK_TVUI_KEYS")) {
            std::string list = keys;
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                const std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
                Step st;
                if (std::sscanf(name.c_str(), "move@%f:%f", &st.x, &st.y) == 2) {
                    st.kind = Step::Move;
                    keys_.push_back(st);
                } else if (std::sscanf(name.c_str(), "click@%f:%f", &st.x, &st.y) == 2) {
                    st.kind = Step::Click;
                    keys_.push_back(st);
                } else if (std::sscanf(name.c_str(), "wheel@%f", &st.y) == 1) {
                    st.kind = Step::Wheel;
                    keys_.push_back(st);
                } else if (sc != SDL_SCANCODE_UNKNOWN) {
                    st.key = sc;
                    keys_.push_back(st);
                } else if (!name.empty()) {
                    WNK_LOGW("script", "unknown key name '%s'", name.c_str());
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        if (const char *shot = SDL_getenv("WNK_TVUI_SHOT")) shot_ = shot;
    }
    bool step(Uint64 frame, wnk::render::Gles2Renderer &r, SDL_Window *window, int dw, int dh) {
        if (keys_.empty() && shot_.empty()) return false;
        const Uint64 first = 20, gap = 6;
        if (frame >= first && next_ < keys_.size() && (frame - first) % gap == 0) {
            const Step &st = keys_[next_];
            // Logical units → drawable pixels → window units, the reverse of the app's input path.
            const wnk::Point px = r.mapping().to_pixels({st.x, st.y});
            int ww = 0, wh = 0;
            SDL_GetWindowSize(window, &ww, &wh);
            const int wx = static_cast<int>(px.x * ww / std::max(1, dw));
            const int wy = static_cast<int>(px.y * wh / std::max(1, dh));
            switch (st.kind) {
                case Step::Key:
                    push(st.key, true);
                    push(st.key, false);
                    break;
                case Step::Move: push_motion(wx, wy); break;
                case Step::Click:
                    push_motion(wx, wy);
                    push_button(wx, wy, true);
                    push_button(wx, wy, false);
                    break;
                case Step::Wheel: push_wheel(static_cast<int>(st.y)); break;
            }
            ++next_;
        }
        if (next_ < keys_.size() || frame < first + keys_.size() * gap + 60) return false;
        if (!shot_.empty()) save(r, dw, dh);
        return true;
    }

 private:
    static void push_motion(int x, int y) {
        SDL_Event ev{};
        ev.type = SDL_MOUSEMOTION;
        ev.motion.x = x;
        ev.motion.y = y;
        ev.motion.timestamp = SDL_GetTicks();
        SDL_PushEvent(&ev);
    }
    static void push_button(int x, int y, bool down) {
        SDL_Event ev{};
        ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        ev.button.x = x;
        ev.button.y = y;
        ev.button.timestamp = SDL_GetTicks();
        SDL_PushEvent(&ev);
    }
    static void push_wheel(int y) {
        SDL_Event ev{};
        ev.type = SDL_MOUSEWHEEL;
        ev.wheel.y = y;
        ev.wheel.timestamp = SDL_GetTicks();
        SDL_PushEvent(&ev);
    }
    static void push(SDL_Scancode sc, bool down) {
        SDL_Event ev{};
        ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        ev.key.keysym.scancode = sc;
        ev.key.keysym.sym = SDL_GetKeyFromScancode(sc);
        ev.key.timestamp = SDL_GetTicks();
        SDL_PushEvent(&ev);
    }
    void save(wnk::render::Gles2Renderer &r, int dw, int dh) {
        std::vector<uint8_t> px = r.read_pixels(0, 0, dw, dh);
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), dw, dh, 32, dw * 4, SDL_PIXELFORMAT_ABGR8888);
        if (s != nullptr && SDL_SaveBMP(s, shot_.c_str()) == 0) {
            WNK_LOGI("script", "screenshot saved to %s (%dx%d)", shot_.c_str(), dw, dh);
        } else {
            WNK_LOGE("script", "screenshot failed: %s", SDL_GetError());
        }
        if (s != nullptr) SDL_FreeSurface(s);
    }
    std::vector<Step> keys_;
    size_t next_ = 0;
    std::string shot_;
};

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

class HomeScreen : public Screen {
 public:
    HomeScreen(const Catalogue &cat, Navigator &nav) : Screen("home"), cat_(cat), nav_(nav) {
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

        const char *names[] = {"Home", "Browse", "Widgets"};
        View *sections[] = {&build_home(), &build_browse(), &build_widgets()};
        for (int i = 0; i < 3; ++i) {
            sections_.push_back(sections[i]);
            auto &item = sidebar.add<NavItem>(names[i], [this, i](Ui &) { show_section(i); });
            item.name = std::string("nav.") + names[i];
            nav_items_.push_back(&item);
        }
        show_section(0);
    }

    View &first_rail() { return *rails_[0]; }

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

    const Catalogue &cat_;
    Navigator &nav_;
    View *content_ = nullptr;
    std::vector<View *> sections_;
    std::vector<NavItem *> nav_items_;
    std::vector<CollectionView *> rails_;
    CollectionView *grid_ = nullptr;
    ProgressBar *progress_ = nullptr;
    int current_ = -1;
};

// ---- app -------------------------------------------------------------------------------------------

class App {
 public:
    App(wnk::render::Renderer &r, Theme theme, const Catalogue &cat, ImageSource &images)
        : ui_(r, theme), mapper_(wnk::input::default_keymap()) {
        ui_.set_image_source(&images);
        nav_ = &ui_.root().add<Navigator>();
        nav_->on_change = [this](Ui &ui, Screen &s) {
            View *f = ui.focused();
            const char *focus = f != nullptr ? f->name.c_str() : "<pending>";
            WNK_LOGI(kCat, "screen %s depth %zu focus=%s", s.title().c_str(), nav_->depth(), focus);
            if (nav_->depth() < depth_) WNK_LOGI(kCat, "back to %s focus=%s", s.title().c_str(), focus);
            depth_ = nav_->depth();
        };
        home_ = &static_cast<HomeScreen &>(nav_->push(ui_, std::make_unique<HomeScreen>(cat, *nav_)));
        ui_.focus_within(&home_->first_rail());
    }

    bool running() const { return running_; }
    Ui &ui() { return ui_; }

    void on_key(const SDL_Event &ev) {
        auto key = wnk::input::key_event_from_sdl(ev);
        if (!key) return;
        if (key->down) {
            const auto *b = mapper_.keymap().find(key->scancode, key->keycode);
            WNK_LOGI("key", "down sc=%d kc=0x%x repeat=%d action=%s pointer=%d", key->scancode, key->keycode,
                     key->repeat ? 1 : 0, b != nullptr ? wnk::input::action_name(b->action) : "unmapped",
                     ui_.pointer_active() ? 1 : 0);
        }
        if (auto a = mapper_.map(*key)) handle(*a);
    }

    void on_pointer(const wnk::input::PointerEvent &p) {
        using T = wnk::input::PointerEvent::Type;
        const bool hit = ui_.handle_pointer(p);
        const wnk::Point l = ui_.pointer_position();
        View *f = ui_.focused();
        const char *name = f != nullptr ? f->name.c_str() : "<none>";
        if (p.type == T::Move) {
            // Rate-limited: motion arrives at the remote's report rate.
            if (p.time_ms - last_move_log_ < 1000 && moves_logged_ > 0) return;
            last_move_log_ = p.time_ms;
            ++moves_logged_;
        }
        WNK_LOGI("pointer", "%s px=(%.0f,%.0f) logical=(%.0f,%.0f) button=%d wheel=%.1f hit=%d focus=%s",
                 wnk::input::pointer_type_name(p.type), p.x, p.y, l.x, l.y, p.button, p.wheel, hit ? 1 : 0, name);
    }

    void on_focus_lost(Uint32 now) {
        for (const auto &a : mapper_.release_all(now)) handle(a);
    }

    void frame(float dt) {
        ui_.update(dt);
        View *f = ui_.focused();
        if (f != last_focus_) {
            last_focus_ = f;
            WNK_LOGI("focus", "focus %s", f != nullptr ? f->name.c_str() : "<none>");
        }
    }

    struct Perf {
        int n = 0, late = 0;  // frames, intervals over 20 ms (a missed vsync at 60 Hz)
        double interval_sum = 0, interval_max = 0;
        double events_max = 0, update_max = 0, draw_max = 0, finish_max = 0, swap_sum = 0, swap_max = 0;
    };
    void perf_log(Uint64 frames, const Perf &p, const wnk::render::RenderStats &rs) {
        const auto s = ui_.stats();
        WNK_LOGI("perf", "frames %llu: interval avg %.2f max %.2f ms, late %d; max ms events %.2f update %.2f "
                 "draw %.2f finish %.2f swap %.2f (avg %.2f); draw calls %u, quads %u, textures %u, views %zu",
                 static_cast<unsigned long long>(frames), p.interval_sum / p.n, p.interval_max, p.late, p.events_max,
                 p.update_max, p.draw_max, p.finish_max, p.swap_max, p.swap_sum / p.n, rs.draw_calls, rs.quads,
                 rs.textures, s.views);
    }

    void shutdown_log() {
        const auto s = ui_.stats();
        WNK_LOGI(kCat, "ui stats: views %zu, focus nodes %zu, layouts %llu, browse grid cells %s", s.views,
                 s.focus_nodes, static_cast<unsigned long long>(s.layouts), home_->describe().c_str());
    }

 private:
    void handle(const wnk::input::ActionEvent &a) {
        if (a.action == Action::Exit && a.phase == wnk::input::Phase::Press) {
            WNK_LOGI(kCat, "Exit key: quitting");
            running_ = false;
            return;
        }
        if (ui_.handle(a)) return;
        if (a.action == Action::Back && a.phase == wnk::input::Phase::Press) {
            const Uint32 now = a.source.time_ms;
            if (last_back_ != 0 && now - last_back_ <= kDoubleBackMs) {
                WNK_LOGI(kCat, "Back twice at root: quitting");
                running_ = false;
                return;
            }
            last_back_ = now;
            ui_.toast("Press Back again to exit", kDoubleBackMs);
        }
    }

    Ui ui_;
    wnk::input::ActionMapper mapper_;
    Navigator *nav_ = nullptr;
    HomeScreen *home_ = nullptr;
    View *last_focus_ = nullptr;
    size_t depth_ = 0;
    Uint32 last_move_log_ = 0;
    int moves_logged_ = 0;
    bool running_ = true;
    Uint32 last_back_ = 0;
};

int fail(const char *what) {
    WNK_LOGE(kCat, "%s: %s", what, SDL_GetError());
    SDL_Quit();
    return 1;
}

}  // namespace

int main(int argc, char **argv) {
    wnk::platform::prepare_process_environment();
    const auto launch = wnk::platform::read_launch_info(argc, argv, kFallbackAppId);
    const std::string runtime = wnk::platform::runtime_dir(launch.app_id);

    wnk::log::Logger logger(wnk::log::Level::Debug);
    logger.add_sink(std::make_unique<wnk::log::StderrSink>());
    if (!runtime.empty()) {
        int fd = wnk::log::open_log_file(runtime + "/events.log", wnk::log::OpenMode::Truncate);
        if (fd >= 0) logger.add_sink(std::make_unique<wnk::log::FileSink>(fd));
    }
    wnk::log::set_default_logger(&logger);
    WNK_LOGI(kCat, "start app_id=%s platform=%s", launch.app_id.c_str(), wnk::platform::kIsWebOS ? "webos" : "host");

    // SDL_Init(0) first: SDL-webOS registers the app with the application manager here.
    if (SDL_Init(0) != 0) return fail("SDL_Init(0)");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
    SDL_SetHint("SDL_WEBOS_CURSOR_SLEEP_TIME", "5000");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return fail("SDL video init");
    SDL_DisplayMode mode{};
    SDL_GetCurrentDisplayMode(0, &mode);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    Uint32 flags = SDL_WINDOW_OPENGL;
    int win_w = 1280, win_h = 720;
    if constexpr (wnk::platform::kIsWebOS) {
        flags |= SDL_WINDOW_FULLSCREEN;
        win_w = mode.w > 0 ? mode.w : 1920;
        win_h = mode.h > 0 ? mode.h : 1080;
    } else {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    SDL_Window *window = SDL_CreateWindow("wnk tvui", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, win_w, win_h, flags);
    if (window == nullptr) return fail("SDL_CreateWindow");
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (gl == nullptr) {
        SDL_DestroyWindow(window);
        return fail("SDL_GL_CreateContext");
    }
    if (SDL_GL_SetSwapInterval(1) != 0) WNK_LOGW("gl", "vsync unavailable: %s", SDL_GetError());

    int exit_code = 0;
    {
        std::string err;
        auto renderer = wnk::render::Gles2Renderer::create(&err);
        Theme theme;
        if (renderer) {
            theme.regular = renderer->load_font_file(launch.app_dir + "/fonts/AtkinsonHyperlegible-Regular.ttf", &err);
            theme.bold = renderer->load_font_file(launch.app_dir + "/fonts/AtkinsonHyperlegible-Bold.ttf", &err);
        }
        if (!renderer || theme.regular == wnk::render::kNoFont || theme.bold == wnk::render::kNoFont) {
            WNK_LOGE(kCat, "renderer or font init failed: %s", err.c_str());
            exit_code = 1;
        } else {
            // Shutdown order (reverse of declaration): app/UI, textures, then the pool is shut down
            // explicitly before the dispatcher goes away.
            wnk::async::MainDispatcher dispatcher;
            wnk::async::WorkerPool pool(2);
            wnk::image::ImageServiceConfig image_cfg;
            image_cfg.memory_bytes = 48u << 20;
            wnk::image::ImageService images(nullptr, pool, dispatcher, image_cfg);  // file:// only
            wnk::image::ImageTextures textures(*renderer, 64u << 20);
            ServiceImageSource source(images, textures);
            Catalogue cat{"file://" + launch.app_dir + "/posters"};
            {
                App app(*renderer, theme, cat, source);
                Script script;
                const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
                Uint64 prev = SDL_GetPerformanceCounter();
                Uint64 frames = 0;
                App::Perf perf;
                auto ms_since = [freq](Uint64 t) { return (SDL_GetPerformanceCounter() - t) * 1000.0 / freq; };
                bool quit = false;
                while (!quit && app.running()) {
                    const Uint64 t_events = SDL_GetPerformanceCounter();
                    SDL_Event ev;
                    while (SDL_PollEvent(&ev)) {
                        switch (ev.type) {
                            case SDL_QUIT: quit = true; break;
                            case SDL_KEYDOWN:
                            case SDL_KEYUP:
                                // SDL-webOS announces the pointer appearing/hiding as pseudo-keys.
                                if (auto p = wnk::input::pointer_event_from_sdl(ev, window)) app.on_pointer(*p);
                                app.on_key(ev);
                                break;
                            case SDL_MOUSEMOTION:
                            case SDL_MOUSEBUTTONDOWN:
                            case SDL_MOUSEBUTTONUP:
                            case SDL_MOUSEWHEEL:
                                if (auto p = wnk::input::pointer_event_from_sdl(ev, window)) app.on_pointer(*p);
                                break;
                            case SDL_WINDOWEVENT:
                                if (auto p = wnk::input::pointer_event_from_sdl(ev, window)) app.on_pointer(*p);
                                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) app.on_focus_lost(SDL_GetTicks());
                                if (ev.window.event == SDL_WINDOWEVENT_CLOSE) quit = true;
                                break;
                            case SDL_APP_WILLENTERBACKGROUND: app.on_focus_lost(SDL_GetTicks()); break;
                            default: break;
                        }
                        if (!app.running()) break;
                    }
                    if (quit || !app.running()) break;
                    perf.events_max = std::max(perf.events_max, ms_since(t_events));
                    const Uint64 now = SDL_GetPerformanceCounter();
                    const float dt = static_cast<float>((now - prev) / freq);
                    prev = now;
                    if (frames > 0) {
                        perf.interval_sum += dt * 1000.0;
                        perf.interval_max = std::max(perf.interval_max, static_cast<double>(dt) * 1000.0);
                        if (dt * 1000.0 > 20.0) ++perf.late;
                    }
                    dispatcher.drain(32, std::chrono::milliseconds(4));
                    app.frame(std::min(dt, 0.1f));
                    perf.update_max = std::max(perf.update_max, ms_since(now));
                    const Uint64 t_draw = SDL_GetPerformanceCounter();
                    int dw = 0, dh = 0;
                    SDL_GL_GetDrawableSize(window, &dw, &dh);
                    renderer->begin_frame(dw, dh);
                    renderer->clear(theme.background);
                    app.ui().draw();
                    perf.draw_max = std::max(perf.draw_max, ms_since(t_draw));
                    const Uint64 t_finish = SDL_GetPerformanceCounter();
                    renderer->end_frame();
                    textures.end_frame();
                    perf.finish_max = std::max(perf.finish_max, ms_since(t_finish));
                    if (script.step(frames, *renderer, window, dw, dh)) quit = true;
                    const Uint64 t_swap = SDL_GetPerformanceCounter();
                    SDL_GL_SwapWindow(window);
                    const double swap_ms = ms_since(t_swap);
                    perf.swap_sum += swap_ms;
                    perf.swap_max = std::max(perf.swap_max, swap_ms);
                    if (++perf.n == 300) {
                        app.perf_log(frames + 1, perf, renderer->stats());
                        perf = App::Perf{};
                    }
                    ++frames;
                    const double frame_ms = (SDL_GetPerformanceCounter() - now) * 1000.0 / freq;
                    if (frame_ms < 4.0) SDL_Delay(static_cast<Uint32>(1000.0 / 60.0 - frame_ms));
                }
                WNK_LOGI(kCat, "shutting down after %llu frames", static_cast<unsigned long long>(frames));
                app.shutdown_log();
            }
            dispatcher.close();
            if (!pool.shutdown(std::chrono::milliseconds(2000))) WNK_LOGW(kCat, "a worker ignored cancellation");
        }
    }
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    WNK_LOGI(kCat, "shutdown complete (exit %d)", exit_code);
    logger.flush();
    wnk::log::set_default_logger(nullptr);
    return exit_code;
}
