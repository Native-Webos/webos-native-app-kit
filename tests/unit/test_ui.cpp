// SPDX-License-Identifier: GPL-3.0-or-later
//
// UI toolkit tests without a GPU: a recording renderer with deterministic text metrics (every code
// point is half the font size wide), real FocusManager, real input events.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/ui/collection.hpp"
#include "wnk/ui/forms.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/ui.hpp"
#include "wnk/ui/widgets.hpp"

#include "wnk/render/font.hpp"

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace wnk;
using namespace wnk::ui;
using input::Action;

namespace {

class FakeRenderer : public render::Renderer {
 public:
    struct Text {
        std::string text;
        Point baseline;
    };
    std::vector<Rect> fills, strokes, images;
    std::vector<Text> texts;
    render::CanvasMapping map;

    void set_logical_size(Size) override {}
    void begin_frame(int, int) override {
        fills.clear();
        strokes.clear();
        images.clear();
        texts.clear();
    }
    const render::CanvasMapping &mapping() const override { return map; }
    void clear(render::Color) override {}
    void push_clip(const Rect &) override {}
    void pop_clip() override {}
    void push_translate(float dx, float dy) override {
        stack_.push_back(t_);
        t_.x += dx;
        t_.y += dy;
    }
    void pop_translate() override {
        t_ = stack_.back();
        stack_.pop_back();
    }
    void fill_rect(const Rect &r, render::Color, float) override { fills.push_back(shift(r)); }
    void stroke_rect(const Rect &r, render::Color, float, float) override { strokes.push_back(shift(r)); }
    void draw_image(render::TextureId, const Rect &dst, render::Color, const Rect &) override {
        images.push_back(shift(dst));
    }
    float draw_text(const render::TextStyle &s, Point b, std::string_view t) override {
        texts.push_back({std::string(t), {b.x + t_.x, b.y + t_.y}});
        return measure_text(s, t);
    }
    float measure_text(const render::TextStyle &s, std::string_view t) const override {
        return static_cast<float>(render::decode_utf8(t).size()) * s.size * 0.5f;
    }
    float line_height(const render::TextStyle &s) const override { return s.size * 1.25f; }
    float ascent(const render::TextStyle &s) const override { return s.size; }
    void end_frame() override {}
    render::TextureId create_texture(int, int, const uint8_t *) override { return 1; }
    bool destroy_texture(render::TextureId) override { return true; }
    render::FontId load_font(std::vector<uint8_t>, std::string *) override { return 1; }
    render::FontId load_font_file(const std::string &, std::string *) override { return 1; }
    render::RenderStats stats() const override { return {}; }
    render::GpuInfo gpu_info() const override { return {}; }

    bool drew_text(const std::string &s) const {
        for (const auto &t : texts) {
            if (t.text == s) return true;
        }
        return false;
    }

 private:
    Rect shift(Rect r) const { return {r.x + t_.x, r.y + t_.y, r.w, r.h}; }
    Point t_;
    std::vector<Point> stack_;
};

struct Env {
    FakeRenderer r;
    Ui ui{r, Theme{}};
    void frame(float dt = 1.0f / 60) {
        ui.update(dt);
        r.begin_frame(1920, 1080);
        ui.draw();
    }
    void frames(int n, float dt = 1.0f / 60) {
        for (int i = 0; i < n; ++i) frame(dt);
    }
    bool press(Action a, input::Phase phase = input::Phase::Press) {
        input::ActionEvent e;
        e.action = a;
        e.phase = phase;
        return ui.handle(e);
    }
};

std::unique_ptr<Button> button(const std::string &text, int *clicks = nullptr) {
    return std::make_unique<Button>(text, [clicks](Ui &) {
        if (clicks != nullptr) ++*clicks;
    });
}

}  // namespace

namespace wnk {
std::ostream &operator<<(std::ostream &os, const Rect &r) {
    return os << "{" << r.x << "," << r.y << "," << r.w << "," << r.h << "}";
}
}  // namespace wnk

// ---- layout ---------------------------------------------------------------------------------------

TEST("row: fixed, auto and fill children share the width with gaps") {
    Env env;
    auto &row = env.ui.root().add<Row>(10);
    row.set_size(1000, 100);
    auto &a = row.add<View>();
    a.set_width(Dim::fixed(200));
    auto &b = row.add<View>();
    b.set_width(Dim::fill(1));
    auto &c = row.add<View>();
    c.set_width(Dim::fill(3));
    env.ui.sync();
    CHECK_EQ(row.frame(), (Rect{0, 0, 1000, 100}));
    CHECK_EQ(a.frame(), (Rect{0, 0, 200, 100}));  // stretched across by default
    CHECK_EQ(b.frame(), (Rect{210, 0, 195, 100}));
    CHECK_EQ(c.frame(), (Rect{415, 0, 585, 100}));
}

TEST("column: padding, cross alignment and justify") {
    Env env;
    auto &col = env.ui.root().add<Column>(20);
    col.set_size(400, 600);
    col.set_padding(Insets::all(10));
    col.set_justify(Align::End);
    auto &a = col.add<View>();
    a.set_size(100, 50).set_align(Align::Center, Align::Start);
    auto &b = col.add<View>();
    b.set_size(100, 50).set_align(Align::End, Align::Start);
    env.ui.sync();
    CHECK_EQ(a.frame(), (Rect{150, 470, 100, 50}));
    CHECK_EQ(b.frame(), (Rect{290, 540, 100, 50}));
}

TEST("box: invisible children take no space") {
    Env env;
    auto &row = env.ui.root().add<Row>(10);
    row.set_size(1000, 100);
    auto &a = row.add<View>();
    a.set_width(Dim::fixed(100));
    auto &hidden = row.add<View>();
    hidden.set_width(Dim::fixed(300)).set_visible(false);
    auto &c = row.add<View>();
    c.set_width(Dim::fixed(100));
    env.ui.sync();
    CHECK_EQ(c.frame().x, 110.0f);
}

TEST("grid: equal columns, rows as tall as their tallest child") {
    Env env;
    auto &grid = env.ui.root().add<Grid>(3, 10);
    grid.set_width(Dim::fixed(320)).set_align(Align::Start, Align::Start);
    std::vector<View *> cells;
    for (int i = 0; i < 5; ++i) {
        auto &v = grid.add<View>();
        v.set_height(Dim::fixed(i == 1 ? 80.0f : 40.0f));
        cells.push_back(&v);
    }
    env.ui.sync();
    CHECK_EQ(cells[0]->frame(), (Rect{0, 0, 100, 40}));
    CHECK_EQ(cells[2]->frame().x, 220.0f);
    CHECK_EQ(cells[3]->frame().y, 90.0f);  // first row is 80 tall (+10 gap)
    CHECK_EQ(grid.frame().h, 130.0f);
}

TEST("label: wraps on spaces, breaks long words, ellipsises the last line") {
    FakeRenderer r;
    render::TextStyle s{1, 20, render::kWhite};  // 10 units per code point
    auto lines = Label::wrap(r, s, "one two three four", 100, 3);
    REQUIRE(lines.size() == 2);
    CHECK_EQ(lines[0], std::string("one two"));
    CHECK_EQ(lines[1], std::string("three four"));
    lines = Label::wrap(r, s, "abcdefghijklmnop", 50, 4);
    REQUIRE(lines.size() == 4);
    CHECK_EQ(lines[0], std::string("abcde"));
    CHECK_EQ(lines[3], std::string("p"));
    lines = Label::wrap(r, s, "one two three four five six", 95, 2);
    REQUIRE(lines.size() == 2);
    CHECK_EQ(lines[0], std::string("one two"));
    CHECK_EQ(lines[1], std::string("three fo\xE2\x80\xA6"));  // 9 code points = 90 <= 95
    lines = Label::wrap(r, s, "first\nsecond", 500, 3);
    REQUIRE(lines.size() == 2);
    CHECK_EQ(lines[1], std::string("second"));
    // Multi-byte code points are never split.
    lines = Label::wrap(r, s, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 20, 3);
    REQUIRE(lines.size() == 2);
    CHECK_EQ(lines[0], std::string("\xC3\xA9\xC3\xA9"));
    CHECK(Label::wrap(r, s, "x", 0, 2).empty());
}

TEST("label: single line measures at natural width and draws at its baseline") {
    Env env;
    auto &l = env.ui.root().add<Label>("Hello");  // body size 32 → 16 per code point
    l.set_align(Align::Start, Align::Start);
    env.frame();
    CHECK_EQ(l.frame().w, 80.0f);
    CHECK_EQ(l.frame().h, 40.0f);
    REQUIRE(env.r.texts.size() == 1);
    CHECK_EQ(env.r.texts[0].baseline.y, 32.0f);
}

// ---- focus and input ------------------------------------------------------------------------------

TEST("focus: buttons register as nodes, arrows move by geometry, OK clicks") {
    Env env;
    int clicks_a = 0, clicks_b = 0;
    auto &row = env.ui.root().add<Row>(20);
    row.set_align(Align::Start, Align::Start);
    auto &a = static_cast<Button &>(row.add_child(button("A", &clicks_a)));
    auto &b = static_cast<Button &>(row.add_child(button("B", &clicks_b)));
    env.ui.focus_within(&env.ui.root());
    env.frame();
    CHECK(env.ui.is_focused(&a));
    CHECK_EQ(env.ui.stats().focus_nodes, size_t{2});
    CHECK(env.press(Action::Right));
    CHECK(env.ui.is_focused(&b));
    CHECK(!env.press(Action::Right));  // nothing further right
    CHECK(env.press(Action::Confirm));
    CHECK(env.press(Action::Confirm, input::Phase::Repeat));  // consumed, but no second click
    CHECK_EQ(clicks_b, 1);
    CHECK_EQ(clicks_a, 0);
}

TEST("focus: hidden and disabled views are skipped; hiding the focused view moves focus") {
    Env env;
    auto &col = env.ui.root().add<Column>(10);
    col.set_align(Align::Start, Align::Start);
    auto &a = static_cast<Button &>(col.add_child(button("A")));
    auto &b = static_cast<Button &>(col.add_child(button("B")));
    auto &c = static_cast<Button &>(col.add_child(button("C")));
    b.set_enabled(false);
    env.ui.focus(&a);
    env.frame();
    CHECK(env.press(Action::Down));
    CHECK(env.ui.is_focused(&c));  // skipped disabled B
    c.set_visible(false);
    env.frame();
    CHECK(env.ui.is_focused(&a));
}

TEST("input: unhandled actions bubble to ancestors; Back at the root is left to the app") {
    struct Catcher : View {
        int reds = 0;
        bool on_action(Ui &, const input::ActionEvent &e) override {
            if (e.action != Action::Red) return false;
            ++reds;
            return true;
        }
    };
    Env env;
    auto &catcher = env.ui.root().add<Catcher>();
    auto &inner = catcher.add<Row>();
    auto &b = static_cast<Button &>(inner.add_child(button("B")));
    env.ui.focus(&b);
    env.frame();
    CHECK(env.press(Action::Red));
    CHECK_EQ(catcher.reds, 1);
    CHECK(!env.press(Action::Back));
}

TEST("focus: a request for a view not yet attached is applied when it is") {
    Env env;
    auto owned = button("Later");
    Button *later = owned.get();
    CHECK(!env.ui.focus(later));
    env.ui.root().add_child(std::move(owned));
    env.frame();
    CHECK(env.ui.is_focused(later));
}

TEST("focus ring: drawn around the focused view and follows it") {
    Env env;
    auto &row = env.ui.root().add<Row>(100);
    row.set_align(Align::Start, Align::Start);
    auto &a = static_cast<Button &>(row.add_child(button("A")));
    auto &b = static_cast<Button &>(row.add_child(button("B")));
    env.ui.focus(&a);
    env.frame();
    REQUIRE(env.r.strokes.size() == 1);
    const float gap = env.ui.theme().ring_gap;
    CHECK_EQ(env.r.strokes[0], a.frame().inset(-gap));
    env.press(Action::Right);
    env.frame();
    CHECK(env.r.strokes[0].x < b.frame().x - gap);  // animating from A towards B
    env.frames(60);
    CHECK_EQ(env.r.strokes[0], b.frame().inset(-gap));
}

// ---- navigation -----------------------------------------------------------------------------------

namespace {
struct ButtonsScreen : Screen {
    std::vector<Button *> buttons;
    explicit ButtonsScreen(int n, const std::string &title = "s") : Screen(title) {
        auto &row = add<Row>(20);
        row.set_align(Align::Start, Align::Start);
        for (int i = 0; i < n; ++i) buttons.push_back(&static_cast<Button &>(row.add_child(button("b" + std::to_string(i)))));
    }
};
}  // namespace

TEST("navigator: push focuses the new screen, Back pops and restores the previous focus") {
    Env env;
    auto &nav = env.ui.root().add<Navigator>();
    auto &home = static_cast<ButtonsScreen &>(nav.push(env.ui, std::make_unique<ButtonsScreen>(4, "home")));
    env.frame();
    CHECK(env.ui.is_focused(home.buttons[0]));
    env.press(Action::Right);
    env.press(Action::Right);
    CHECK(env.ui.is_focused(home.buttons[2]));

    auto &details = static_cast<ButtonsScreen &>(nav.push(env.ui, std::make_unique<ButtonsScreen>(2, "details")));
    env.frame();
    CHECK(env.ui.is_focused(details.buttons[0]));
    CHECK(!home.visible());
    // The covered screen's nodes stay registered but cannot take focus.
    CHECK(!env.press(Action::Left));
    CHECK(env.ui.is_focused(details.buttons[0]));

    CHECK(env.press(Action::Back));
    env.frames(30);  // leaving animation ends, the screen is destroyed
    CHECK_EQ(nav.depth(), size_t{1});
    CHECK(env.ui.is_focused(home.buttons[2]));
    CHECK(!env.press(Action::Back));  // bottom of the stack: the app decides
}

TEST("navigator: a click handler may pop the screen its button lives on") {
    Env env;
    auto &nav = env.ui.root().add<Navigator>();
    nav.push(env.ui, std::make_unique<ButtonsScreen>(1));
    auto second = std::make_unique<Screen>("second");
    auto &close = static_cast<Button &>(second->add_child(button("close")));
    close.set_align(Align::Start, Align::Start);
    close.set_on_click([&nav](Ui &ui) { nav.pop(ui); });
    nav.push(env.ui, std::move(second));
    env.frame();
    CHECK(env.ui.is_focused(&close));
    CHECK(env.press(Action::Confirm));  // `close` must stay alive until the handler returns (ASan)
    env.frames(30);
    CHECK_EQ(nav.depth(), size_t{1});
    CHECK_EQ(env.ui.stats().retired, size_t{0});
}

TEST("navigator: nested scope memory survives a push and pop") {
    Env env;
    auto &nav = env.ui.root().add<Navigator>();
    auto home = std::make_unique<Screen>("home");
    auto &col = home->add<Column>(40);
    col.set_align(Align::Start, Align::Start);
    std::vector<std::vector<Button *>> rows(2);
    for (int r = 0; r < 2; ++r) {
        auto &row = col.add<Row>(20);
        row.set_focus_scope();
        for (int i = 0; i < 3; ++i) rows[r].push_back(&static_cast<Button &>(row.add_child(button("r" + std::to_string(r)))));
    }
    nav.push(env.ui, std::move(home));
    env.frame();
    env.press(Action::Down);   // row 1, column 0
    env.press(Action::Right);  // row 1, column 1
    env.press(Action::Up);     // row 0 remembers column 0
    CHECK(env.ui.is_focused(rows[0][0]));
    env.press(Action::Right);
    env.press(Action::Right);  // row 0, column 2
    nav.push(env.ui, std::make_unique<ButtonsScreen>(1));
    env.frame();
    env.press(Action::Back);
    env.frames(20);
    CHECK(env.ui.is_focused(rows[0][2]));
    env.press(Action::Down);  // row 1 still remembers column 1
    CHECK(env.ui.is_focused(rows[1][1]));
}

// ---- dialogs and toasts ---------------------------------------------------------------------------

TEST("dialog: traps focus, Back dismisses it and focus returns") {
    Env env;
    auto &screen = env.ui.root().add<ButtonsScreen>(3);
    env.ui.focus(screen.buttons[1]);
    env.frame();
    bool dismissed = false;
    auto d = std::make_unique<Dialog>("Delete?", "This cannot be undone.");
    auto &cancel = d->add_button("Cancel");
    auto &ok = d->add_button("Delete");
    d->on_dismiss = [&dismissed](Ui &) { dismissed = true; };
    env.ui.show_dialog(std::move(d));
    env.frame();
    CHECK(env.ui.is_focused(&cancel));
    CHECK(env.press(Action::Right));
    CHECK(env.ui.is_focused(&ok));
    CHECK(!env.press(Action::Right));
    CHECK(!env.press(Action::Down));  // cannot leave the dialog
    CHECK(env.ui.is_focused(&ok));
    CHECK(env.press(Action::Back));
    env.frame();
    CHECK(dismissed);
    CHECK_EQ(env.ui.dialog_count(), size_t{0});
    CHECK(env.ui.is_focused(screen.buttons[1]));
}

TEST("dialog: a button runs its handler and dismisses; the handler may open another dialog") {
    Env env;
    auto &screen = env.ui.root().add<ButtonsScreen>(2);
    env.ui.focus(screen.buttons[1]);
    env.frame();
    int confirmed = 0;
    auto d = std::make_unique<Dialog>("First");
    d->add_button("Next", [&confirmed](Ui &ui) {
        ++confirmed;
        auto second = std::make_unique<Dialog>("Second");
        second->add_button("Done");
        ui.show_dialog(std::move(second));
    });
    env.ui.show_dialog(std::move(d));
    env.frame();
    CHECK(env.press(Action::Confirm));
    env.frame();
    CHECK_EQ(confirmed, 1);
    CHECK_EQ(env.ui.dialog_count(), size_t{1});
    CHECK(env.press(Action::Confirm));  // "Done"
    env.frame();
    CHECK_EQ(env.ui.dialog_count(), size_t{0});
    CHECK(env.ui.is_focused(screen.buttons[1]));
}

TEST("dialog: Back never reaches the navigator underneath") {
    Env env;
    auto &nav = env.ui.root().add<Navigator>();
    nav.push(env.ui, std::make_unique<ButtonsScreen>(1));
    nav.push(env.ui, std::make_unique<ButtonsScreen>(1));
    env.frame();
    auto d = std::make_unique<Dialog>("Sticky");
    d->back_dismisses = false;
    d->add_button("OK");
    env.ui.show_dialog(std::move(d));
    env.frame();
    CHECK(env.press(Action::Back));
    CHECK_EQ(nav.depth(), size_t{2});
    CHECK_EQ(env.ui.dialog_count(), size_t{1});
}

TEST("toast: shown, then gone after its duration") {
    Env env;
    env.ui.toast("Saved", 1000);
    env.frame();
    env.frame();
    CHECK(env.r.drew_text("Saved"));
    env.frames(70);
    CHECK(!env.ui.toast_visible());
    CHECK(!env.r.drew_text("Saved"));
}

// ---- collections ----------------------------------------------------------------------------------

namespace {
struct Grid1000 {
    Env env;
    CollectionView *grid = nullptr;
    std::vector<size_t> activated;
    int created = 0;
    explicit Grid1000(size_t count = 1000, int cols = 5) {
        auto &g = env.ui.root().add<CollectionView>(Axis::Vertical, cols, Size{200, 300}, 20);
        g.set_size(cols * 220.0f + 12, 700);
        g.set_align(Align::Start, Align::Start);
        g.set_items(count, [this](Ui &, size_t i) {
            ++created;
            auto b = std::make_unique<Button>(std::to_string(i));
            return b;
        });
        g.on_activate = [this](Ui &, size_t i) { activated.push_back(i); };
        grid = &g;
        env.frame();
        env.ui.focus_within(grid);
        env.frame();
    }
};
}  // namespace

TEST("collection: only the visible window of a 1000-item grid exists") {
    Grid1000 t;
    CHECK_EQ(t.grid->focused_index(), size_t{0});
    // 700 tall, lines of 320: lines 0-2 visible, plus one line of overscan below = 4 lines of 5.
    CHECK(t.grid->live_count() <= 20);
    CHECK(t.grid->live_count() >= 15);
    CHECK(t.env.ui.stats().views < 40);
}

TEST("collection: arrows move by index, scroll keeps focus visible, window stays bounded") {
    Grid1000 t;
    for (int i = 0; i < 40; ++i) {
        CHECK(t.env.press(Action::Down, i == 0 ? input::Phase::Press : input::Phase::Repeat));
    }
    CHECK_EQ(t.grid->focused_index(), size_t{200});
    t.env.frames(60);
    const float target = t.grid->scroll_target();
    CHECK(std::fabs(t.grid->scroll_position() - target) < 0.5f);
    View *f = t.env.ui.focused();
    REQUIRE(f != nullptr);
    const Rect on_screen = f->screen_rect();
    const Rect vp = t.grid->frame();
    CHECK(on_screen.y >= vp.y);
    CHECK(on_screen.bottom() <= vp.bottom());
    CHECK(t.grid->live_count() <= 25);
    CHECK(t.env.ui.stats().retired == 0);
}

TEST("collection: rapid presses within one frame never skip or escape") {
    Grid1000 t;
    for (int i = 0; i < 12; ++i) t.env.press(Action::Down, input::Phase::Repeat);  // no frames in between
    CHECK_EQ(t.grid->focused_index(), size_t{60});
    CHECK(t.env.ui.has_focus_within(t.grid));
}

TEST("collection: row edges are left to geometric focus; a short last line is reachable") {
    Grid1000 t(13, 5);  // lines: 0-4, 5-9, 10-12
    t.env.press(Action::Right);
    t.env.press(Action::Right);
    t.env.press(Action::Right);
    t.env.press(Action::Right);
    CHECK_EQ(t.grid->focused_index(), size_t{4});
    CHECK(!t.env.press(Action::Right));  // no node to the right: unhandled
    CHECK(t.env.press(Action::Down));
    CHECK_EQ(t.grid->focused_index(), size_t{9});
    CHECK(t.env.press(Action::Down));
    CHECK_EQ(t.grid->focused_index(), size_t{12});  // last line is shorter
    CHECK(!t.env.press(Action::Down));
    CHECK(t.env.press(Action::Confirm));
    REQUIRE(t.activated.size() == 1);
    CHECK_EQ(t.activated[0], size_t{12});
}

TEST("collection: replacing the items keeps focus on the same index") {
    Grid1000 t;
    for (int i = 0; i < 3; ++i) t.env.press(Action::Down);
    CHECK_EQ(t.grid->focused_index(), size_t{15});
    t.grid->set_items(500, [](Ui &, size_t i) { return std::make_unique<Button>("new " + std::to_string(i)); });
    t.env.frames(3);
    CHECK_EQ(t.grid->focused_index(), size_t{15});
    auto *b = dynamic_cast<Button *>(t.env.ui.focused());
    REQUIRE(b != nullptr);
    CHECK_EQ(b->text(), std::string("new 15"));
    t.grid->set_items(4, [](Ui &, size_t i) { return std::make_unique<Button>("few " + std::to_string(i)); });
    t.env.frames(3);
    CHECK_EQ(t.grid->focused_index(), size_t{3});
    CHECK(t.env.ui.has_focus_within(t.grid));
}

TEST("collection: horizontal rails keep their own position (row memory)") {
    Env env;
    auto &col = env.ui.root().add<Column>(30);
    col.set_align(Align::Start, Align::Start);
    std::vector<CollectionView *> rails;
    for (int r = 0; r < 2; ++r) {
        auto &rail = col.add<CollectionView>(Axis::Horizontal, 1, Size{200, 300}, 20);
        rail.set_width(Dim::fixed(1200));
        rail.set_items(50, [](Ui &, size_t i) { return std::make_unique<Button>(std::to_string(i)); });
        rails.push_back(&rail);
    }
    env.frame();
    env.ui.focus_within(rails[0]);
    for (int i = 0; i < 7; ++i) env.press(Action::Right);
    env.frames(30);
    CHECK_EQ(rails[0]->focused_index(), size_t{7});
    CHECK(rails[0]->scroll_position() > 0);
    CHECK(env.press(Action::Down));
    CHECK(env.ui.has_focus_within(rails[1]));
    env.press(Action::Right);
    env.frames(5);
    CHECK(env.press(Action::Up));
    CHECK(env.ui.has_focus_within(rails[0]));
    CHECK_EQ(rails[0]->focused_index(), size_t{7});
}

// ---- images and misc ------------------------------------------------------------------------------

namespace {
struct FakeImages : ImageSource {
    struct Pending {
        Ticket ticket;
        std::string url;
        int w, h;
        std::function<void(Loaded)> done;
    };
    std::vector<Pending> pending;
    std::vector<Ticket> cancelled;
    bool evicted = false;
    Ticket next = 1;
    Ticket load(const std::string &url, int w, int h, std::function<void(Loaded)> done) override {
        pending.push_back({next, url, w, h, std::move(done)});
        return next++;
    }
    void cancel(Ticket t) override { cancelled.push_back(t); }
    render::TextureId texture(const std::string &) override { return evicted ? render::kNoTexture : 7; }
    void complete(bool ok) {
        auto p = std::move(pending);
        pending.clear();
        for (auto &x : p) {
            Loaded l;
            l.ok = ok;
            l.key = x.url;
            l.width = 300;
            l.height = 450;
            x.done(l);
        }
    }
};
}  // namespace

TEST("image view: loads once laid out, draws the texture, cancels when destroyed") {
    Env env;
    FakeImages images;
    env.ui.set_image_source(&images);
    auto &img = env.ui.root().add<ImageView>("file:///poster.png");
    img.set_size(200, 300).set_align(Align::Start, Align::Start);
    env.frame();
    REQUIRE(images.pending.size() == 1);
    CHECK_EQ(images.pending[0].w, 300);  // cover: larger side both ways
    CHECK(img.state() == ImageView::State::Loading);
    CHECK(env.r.images.empty());
    images.complete(true);
    env.frame();
    CHECK(img.state() == ImageView::State::Ready);
    REQUIRE(env.r.images.size() == 1);
    CHECK_EQ(env.r.images[0], (Rect{0, 0, 200, 300}));

    images.evicted = true;  // pixels gone before upload: load again
    env.frame();
    env.frame();
    CHECK_EQ(images.pending.size(), size_t{1});

    env.ui.root().remove_child(&img);
    env.frame();
    REQUIRE(images.cancelled.size() == 1);
    CHECK_EQ(images.cancelled[0], images.pending[0].ticket);
}

TEST("scroll view: focusing a view below the fold scrolls it into view") {
    Env env;
    auto &sv = env.ui.root().add<ScrollView>();
    sv.set_size(800, 400).set_align(Align::Start, Align::Start);
    auto &col = sv.add<Column>(20);
    std::vector<Button *> bs;
    for (int i = 0; i < 10; ++i) bs.push_back(&static_cast<Button &>(col.add_child(button("b"))));
    env.ui.focus(bs[0]);
    env.frame();
    CHECK(sv.max_scroll() > 0);
    for (int i = 0; i < 9; ++i) env.press(Action::Down);
    CHECK(env.ui.is_focused(bs[9]));
    env.frames(90);
    const Rect r = bs[9]->screen_rect();
    CHECK(r.bottom() <= 400);
    CHECK(r.y >= 0);
}

TEST("progress bar and animation helpers move towards their targets") {
    Env env;
    auto &p = env.ui.root().add<ProgressBar>();
    p.set_value(0.5f);
    env.frames(120);
    CHECK(std::fabs(p.shown_value() - 0.5f) < 0.001f);
    Smoothed s;
    s.target = 100;
    for (int i = 0; i < 120; ++i) s.step(1.0f / 60);
    CHECK(s.settled());
    Tween tw;
    tw.start(0, 10, 1000, 200);
    CHECK_EQ(tw.value(900), 0.0f);
    CHECK_EQ(tw.value(1200), 10.0f);
    CHECK(tw.value(1100) > 5.0f);  // ease-out: past halfway at half time
}

// ---- pointer (Magic Remote) -----------------------------------------------------------------------

namespace {
input::PointerEvent pev(input::PointerEvent::Type t, float wheel = 0) {
    input::PointerEvent e;
    e.type = t;
    e.wheel = wheel;
    return e;
}
bool move(Env &env, Point p) { return env.ui.handle_pointer_logical(pev(input::PointerEvent::Type::Move), p); }
bool click(Env &env, Point p) {
    env.ui.handle_pointer_logical(pev(input::PointerEvent::Type::Press), p);
    return env.ui.handle_pointer_logical(pev(input::PointerEvent::Type::Release), p);
}
Point centre(const View &v) {
    const Rect r = v.screen_rect();
    return {r.cx(), r.cy()};
}
}  // namespace

TEST("pointer: hovering focuses, a click is OK, empty space changes nothing") {
    Env env;
    int clicks = 0;
    auto &row = env.ui.root().add<Row>(100);
    row.set_align(Align::Start, Align::Start);
    auto &a = static_cast<Button &>(row.add_child(button("A")));
    auto &b = static_cast<Button &>(row.add_child(button("B", &clicks)));
    env.ui.focus(&a);
    env.frame();
    CHECK(move(env, centre(b)));
    CHECK(env.ui.is_focused(&b));
    CHECK(env.ui.focus_source() == FocusSource::Program);  // only during the hover itself
    CHECK(!move(env, {1800, 1000}));
    CHECK(env.ui.is_focused(&b));  // focus stays where it was
    CHECK(click(env, centre(a)));
    CHECK(env.ui.is_focused(&a));
    CHECK_EQ(clicks, 0);
    CHECK(click(env, centre(b)));
    CHECK_EQ(clicks, 1);
    // Press on one view, release on another: no click.
    env.ui.handle_pointer_logical(pev(input::PointerEvent::Type::Press), centre(a));
    CHECK(!env.ui.handle_pointer_logical(pev(input::PointerEvent::Type::Release), centre(b)));
    CHECK_EQ(clicks, 1);
}

TEST("pointer: an arrow key hands control back to the D-pad") {
    Env env;
    auto &row = env.ui.root().add<Row>(100);
    row.set_align(Align::Start, Align::Start);
    auto &a = static_cast<Button &>(row.add_child(button("A")));
    auto &b = static_cast<Button &>(row.add_child(button("B")));
    env.ui.focus(&a);
    env.frame();
    move(env, centre(a));
    CHECK(env.ui.pointer_active());
    env.press(Action::Right);
    CHECK(!env.ui.pointer_active());
    env.frames(3);  // no re-hover pulling focus back under the stationary pointer
    CHECK(env.ui.is_focused(&b));
    env.ui.handle_pointer_logical(pev(input::PointerEvent::Type::Shown), {});
    CHECK(env.ui.pointer_active());
    env.ui.handle_pointer_logical(pev(input::PointerEvent::Type::Hidden), {});
    CHECK(!env.ui.pointer_active());
}

TEST("pointer: cells clipped out of a collection cannot be hovered; hover does not scroll") {
    Grid1000 t;
    // The overscan line below the viewport exists but is clipped away.
    View *below = t.grid->cell(15);
    REQUIRE(below != nullptr);
    const Point under = centre(*below);
    CHECK(under.y > t.grid->frame().bottom());
    CHECK(t.env.ui.hit_focusable(under) == nullptr);
    // A partly visible cell at the bottom edge can be hovered, but the grid does not scroll to it.
    View *edge = t.grid->cell(10);
    REQUIRE(edge != nullptr);
    const Rect er = edge->screen_rect();
    CHECK(er.bottom() > t.grid->frame().bottom());
    CHECK(move(t.env, {er.cx(), er.y + 10}));
    CHECK_EQ(t.grid->focused_index(), size_t{10});
    t.env.frames(30);
    CHECK_EQ(t.grid->scroll_target(), 0.0f);
    // The same cell reached with the D-pad scrolls into view.
    t.env.press(Action::Up);
    t.env.press(Action::Down);
    CHECK(t.grid->scroll_target() > 0);
}

TEST("pointer: the wheel scrolls the collection under the pointer and focus follows the pointer") {
    Grid1000 t;
    View *c0 = t.grid->cell(0);
    REQUIRE(c0 != nullptr);
    const Point p = centre(*c0);
    move(t.env, p);
    CHECK(t.env.ui.handle_pointer_logical(pev(input::PointerEvent::Type::Wheel, -3), {}));
    CHECK(t.grid->scroll_target() > 0);
    t.env.frames(60);
    // Three lines scrolled: the cell now under the pointer is three lines further on.
    CHECK_EQ(t.grid->focused_index(), size_t{15});
    t.env.ui.handle_pointer_logical(pev(input::PointerEvent::Type::Wheel, 10), {});
    t.env.frames(60);
    CHECK_EQ(t.grid->scroll_target(), 0.0f);  // clamped at the start
    CHECK_EQ(t.grid->focused_index(), size_t{0});
}

TEST("pointer: with a dialog open only the dialog can be hovered or clicked") {
    Env env;
    int clicks = 0;
    auto &screen = env.ui.root().add<ButtonsScreen>(3);
    env.ui.focus(screen.buttons[0]);
    env.frame();
    auto d = std::make_unique<Dialog>("Sure?");
    auto &ok = d->add_button("OK", [&clicks](Ui &) { ++clicks; });
    env.ui.show_dialog(std::move(d));
    env.frames(20);
    CHECK(!move(env, centre(*screen.buttons[2])));
    CHECK(env.ui.is_focused(&ok));
    CHECK(click(env, centre(ok)));
    env.frame();
    CHECK_EQ(clicks, 1);
    CHECK_EQ(env.ui.dialog_count(), size_t{0});
    CHECK(env.ui.is_focused(screen.buttons[0]));
}

TEST("pointer: clicking a card in a rail activates that item") {
    Env env;
    auto &rail = env.ui.root().add<CollectionView>(Axis::Horizontal, 1, Size{200, 300}, 20);
    rail.set_width(Dim::fixed(1200)).set_align(Align::Start, Align::Start);
    rail.set_items(30, [](Ui &, size_t i) { return std::make_unique<Button>(std::to_string(i)); });
    std::vector<size_t> hits;
    rail.on_activate = [&hits](Ui &, size_t i) { hits.push_back(i); };
    env.frames(2);
    View *c3 = rail.cell(3);
    REQUIRE(c3 != nullptr);
    CHECK(click(env, centre(*c3)));
    REQUIRE(hits.size() == 1);
    CHECK_EQ(hits[0], size_t{3});
    // The wheel over a horizontal rail is left for an outer (vertical) scroller.
    CHECK(!env.ui.handle_pointer_logical(pev(input::PointerEvent::Type::Wheel, -1), {}));
}

TEST("spring: starts gently, never overshoots, keeps velocity when retargeted, dt-independent") {
    Smoothed a;
    a.rate = 20;
    a.target = 100;
    a.step(1.0f / 60);
    CHECK(a.value > 0);
    CHECK(a.value < 100 * (1 - std::exp(-20.0f / 60)));  // slower start than an exponential approach
    float prev = a.value;
    for (int i = 0; i < 120; ++i) {
        a.step(1.0f / 60);
        CHECK(a.value <= 100.0f);
        CHECK(a.value >= prev);
        prev = a.value;
    }
    CHECK(a.settled());
    // One 1/30 s step lands where two 1/60 s steps do.
    Smoothed b, c;
    b.rate = c.rate = 20;
    b.target = c.target = 50;
    b.step(1.0f / 30);
    c.step(1.0f / 60);
    c.step(1.0f / 60);
    CHECK(std::fabs(b.value - c.value) < 0.01f);
    // Retargeting mid-flight keeps the velocity.
    Smoothed d;
    d.rate = 20;
    d.target = 100;
    for (int i = 0; i < 5; ++i) d.step(1.0f / 60);
    const float v = d.velocity;
    d.target = 200;
    CHECK_EQ(d.velocity, v);
}

// ---- text field -----------------------------------------------------------------------------------

TEST("text field: OK starts editing and claims text input; typed text, Backspace and arrows edit it") {
    Env env;
    auto &col = env.ui.root().add<Column>(10);
    auto &field = col.add<TextField>("Search");
    field.name = "search";
    auto &after = col.add<Button>("Go", [](Ui &) {});
    int changes = 0;
    field.set_on_change([&changes](Ui &, const std::string &) { ++changes; });
    env.ui.focus(&field);
    env.frame();
    CHECK(env.r.drew_text("Search"));  // placeholder while empty
    CHECK(env.ui.text_input_view() == nullptr);
    CHECK(env.press(Action::Confirm));
    CHECK(field.editing());
    CHECK(env.ui.text_input_view() == &field);
    CHECK(env.ui.handle_text("caf\xC3\xA9"));  // café: the é is two bytes
    CHECK(env.ui.handle_text(" au lait"));
    CHECK_EQ(field.text(), std::string("caf\xC3\xA9 au lait"));
    CHECK_EQ(field.cursor(), field.text().size());
    for (int i = 0; i < 9; ++i) env.ui.handle_edit_key(input::EditKey::Left);
    CHECK_EQ(field.cursor(), size_t{3});  // before the é, not inside it
    env.ui.handle_edit_key(input::EditKey::Delete);
    CHECK_EQ(field.text(), std::string("caf au lait"));
    env.ui.handle_edit_key(input::EditKey::Right);
    env.ui.handle_edit_key(input::EditKey::Backspace);
    CHECK_EQ(field.text(), std::string("cafau lait"));
    env.ui.handle_edit_key(input::EditKey::Home);
    env.ui.handle_text("\xE2\x9C\x93");  // ✓ inserted at the start
    CHECK_EQ(field.text(), std::string("\xE2\x9C\x93" "cafau lait"));
    CHECK_EQ(changes, 5);
    env.ui.handle_edit_key(input::EditKey::End);
    for (int i = 0; i < 3; ++i) env.ui.handle_edit_key(input::EditKey::Backspace);
    CHECK_EQ(field.text(), std::string("\xE2\x9C\x93" "cafau l"));
    env.frame();
    CHECK(env.r.drew_text("\xE2\x9C\x93" "cafau l"));
    // Down leaves the field: editing ends with the text kept.
    CHECK(env.press(Action::Down));
    CHECK(env.ui.is_focused(&after));
    CHECK(!field.editing());
    CHECK(env.ui.text_input_view() == nullptr);
    CHECK(!env.ui.handle_text("x"));
    CHECK_EQ(field.text(), std::string("\xE2\x9C\x93" "cafau l"));
}

TEST("text field: Enter submits and ends editing; Back ends editing without submitting") {
    Env env;
    auto &field = env.ui.root().add<TextField>();
    std::vector<std::string> submitted;
    field.set_on_submit([&submitted](Ui &, const std::string &t) { submitted.push_back(t); });
    env.ui.focus(&field);
    env.press(Action::Confirm);
    env.ui.handle_text("hello");
    env.ui.handle_edit_key(input::EditKey::Enter);
    CHECK(!field.editing());
    REQUIRE(submitted.size() == 1u);
    CHECK_EQ(submitted[0], std::string("hello"));
    // OK as an action (no shell routing) submits too.
    env.press(Action::Confirm);
    env.ui.handle_text("!");
    env.press(Action::Confirm);
    REQUIRE(submitted.size() == 2u);
    CHECK_EQ(submitted[1], std::string("hello!"));
    // Back while editing is consumed by the field; at rest it bubbles.
    env.press(Action::Confirm);
    CHECK(env.press(Action::Back));
    CHECK(!field.editing());
    CHECK(!env.press(Action::Back));
    CHECK_EQ(submitted.size(), size_t{2});
}

TEST("text field: control characters are dropped, max length cuts input, password shows bullets") {
    Env env;
    auto &field = env.ui.root().add<TextField>();
    field.set_max_length(6).set_password(true);
    env.ui.focus(&field);
    env.press(Action::Confirm);
    env.ui.handle_text("a\nb\tc\x7F");
    CHECK_EQ(field.text(), std::string("abc"));
    env.ui.handle_text("d\xC3\xA9" "fgh");  // only three more code points fit
    CHECK_EQ(field.text(), std::string("abcd\xC3\xA9" "f"));
    CHECK_EQ(field.display_text(), std::string("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"));
    env.frame();
    CHECK(!env.r.drew_text("abcd\xC3\xA9" "f"));  // the text itself is never drawn
    env.ui.handle_edit_key(input::EditKey::Clear);
    CHECK(field.text().empty());
    field.set_text("toolongvalue");
    CHECK_EQ(field.text(), std::string("toolon"));
}

TEST("text field: composition is shown until committed; a closed keyboard ends editing") {
    Env env;
    auto &field = env.ui.root().add<TextField>();
    env.ui.focus(&field);
    env.press(Action::Confirm);
    env.ui.handle_composition("\xE3\x84\xB1");  // ㄱ being composed
    CHECK_EQ(field.composition(), std::string("\xE3\x84\xB1"));
    CHECK(field.text().empty());
    env.frame();
    CHECK(env.r.drew_text("\xE3\x84\xB1"));
    env.ui.handle_text("\xEA\xB0\x80");  // 가 committed
    CHECK(field.composition().empty());
    CHECK_EQ(field.text(), std::string("\xEA\xB0\x80"));
    env.ui.text_input_closed();
    CHECK(!field.editing());
    CHECK(env.ui.text_input_view() == nullptr);
    CHECK(env.ui.is_focused(&field));  // focus stays: OK opens the keyboard again
    env.press(Action::Confirm);
    CHECK(field.editing());
}

TEST("text field: removing the editing field releases text input; long text scrolls to the caret") {
    Env env;
    auto &col = env.ui.root().add<Column>();
    auto &field = col.add<TextField>();
    field.set_width(Dim::fixed(400));  // content 344 wide: 21 characters of 16 in the fake metrics
    env.ui.focus(&field);
    env.press(Action::Confirm);
    env.ui.handle_text("abcdefghijklmnopqrstuvwxyz0123456789");
    env.frame();
    REQUIRE(!env.r.texts.empty());
    // The text before the caret is drawn shifted left so its end (the caret) stays inside the field.
    const auto &before = env.r.texts.front();
    const float end = before.baseline.x + 36 * 16;
    const Rect c = field.content_rect();
    CHECK(end <= c.x + c.w);
    CHECK(end >= c.x + c.w - 20);
    col.remove_child(&field);
    CHECK(env.ui.text_input_view() == nullptr);
    CHECK(!env.ui.handle_text("x"));
}

// ---- form controls --------------------------------------------------------------------------------

TEST("forms: toggle flips on OK and reports; set_on does not report") {
    Env env;
    auto &col = env.ui.root().add<Column>(10);
    std::vector<bool> seen;
    auto &t = col.add<Toggle>("Subtitles", false, [&seen](Ui &, bool on) { seen.push_back(on); });
    env.ui.focus(&t);
    CHECK(env.press(Action::Confirm));
    CHECK(t.on());
    env.press(Action::Confirm, input::Phase::Release);
    env.press(Action::Confirm);
    CHECK(!t.on());
    t.set_on(true);
    REQUIRE(seen.size() == 2u);
    CHECK(seen[0] && !seen[1]);
    env.frames(30);
    CHECK(env.r.drew_text("Subtitles"));
    CHECK(!env.press(Action::Left));  // a toggle leaves Left/Right to focus movement
}

TEST("forms: slider steps with Left/Right, repeats, clamps and snaps; formats its value") {
    Env env;
    auto &col = env.ui.root().add<Column>(10);
    std::vector<float> seen;
    auto &s = col.add<Slider>("Volume", 0, 10, 0.5f, 3.3f, [&seen](Ui &, float v) { seen.push_back(v); });
    CHECK_EQ(s.value(), 3.5f);  // snapped to the step
    CHECK_EQ(s.value_text(), std::string("3.5"));
    env.ui.focus(&s);
    CHECK(env.press(Action::Right));
    CHECK(env.press(Action::Right, input::Phase::Repeat));
    CHECK_EQ(s.value(), 4.5f);
    for (int i = 0; i < 30; ++i) env.press(Action::Right, input::Phase::Repeat);
    CHECK_EQ(s.value(), 10.0f);
    const size_t at_max = seen.size();
    CHECK(env.press(Action::Right));  // consumed at the end, no report
    CHECK_EQ(seen.size(), at_max);
    CHECK(env.ui.is_focused(&s));
    s.set_format([](float v) { return std::to_string(static_cast<int>(v * 10)) + " %"; });
    CHECK_EQ(s.value_text(), std::string("100 %"));
    env.press(Action::Left);
    CHECK_EQ(s.value(), 9.5f);
    env.frames(20);
    CHECK(env.r.drew_text("95 %"));
}

TEST("forms: choice steps with Left/Right and OK opens the list on the current option") {
    Env env;
    auto &col = env.ui.root().add<Column>(10);
    std::vector<int> seen;
    auto &c = col.add<Choice>("Quality", std::vector<std::string>{"Auto", "1080p", "720p"}, 0,
                              [&seen](Ui &, int i) { seen.push_back(i); });
    c.name = "quality";
    env.ui.focus(&c);
    env.press(Action::Left);  // already first: no change
    CHECK(seen.empty());
    env.press(Action::Right);
    CHECK_EQ(c.selected(), 1);
    env.frame();
    CHECK(env.r.drew_text("1080p"));
    CHECK(env.press(Action::Confirm));
    REQUIRE(env.ui.top_dialog() != nullptr);
    CHECK_EQ(env.ui.focused()->name, std::string("quality.option1"));
    env.press(Action::Down);
    CHECK_EQ(env.ui.focused()->name, std::string("quality.option2"));
    env.press(Action::Confirm);
    env.frames(30);
    CHECK(env.ui.top_dialog() == nullptr);
    CHECK_EQ(c.selected(), 2);
    REQUIRE(seen.size() == 2u);
    CHECK_EQ(seen[1], 2);
    CHECK(env.ui.is_focused(&c));  // focus returns to the row
}

TEST("forms: value row runs its handler; tabs follow key focus but not pointer hover") {
    Env env;
    auto &col = env.ui.root().add<Column>(10);
    int opened = 0;
    std::vector<int> pages;
    auto &tabs = col.add<Tabs>(std::vector<std::string>{"General", "Playback", "Account"}, 0,
                               [&pages](Ui &, int i) { pages.push_back(i); });
    auto &row = col.add<ValueRow>("Server", "192.168.1.20", [&opened](Ui &) { ++opened; });
    env.ui.focus_within(&tabs);
    env.frame();
    CHECK(env.r.drew_text("192.168.1.20"));
    env.press(Action::Right);
    env.press(Action::Right);
    CHECK_EQ(tabs.selected(), 2);
    REQUIRE(pages.size() == 2u);
    env.press(Action::Down);
    CHECK(env.ui.is_focused(&row));
    env.press(Action::Confirm);
    CHECK_EQ(opened, 1);
    env.press(Action::Up);  // back into the tabs: the remembered tab, no new selection
    CHECK_EQ(tabs.selected(), 2);
    CHECK_EQ(pages.size(), size_t{2});
    // Pointer: hovering a tab focuses it without selecting; a click selects.
    env.frame();
    View *first = env.ui.focused()->parent()->children()[0].get();
    const Rect r = first->screen_rect();
    input::PointerEvent move;
    move.type = input::PointerEvent::Type::Move;
    env.ui.handle_pointer_logical(move, {r.cx(), r.cy()});
    CHECK(env.ui.is_focused(first));
    CHECK_EQ(tabs.selected(), 2);
    input::PointerEvent press = move, release = move;
    press.type = input::PointerEvent::Type::Press;
    release.type = input::PointerEvent::Type::Release;
    env.ui.handle_pointer_logical(press, {r.cx(), r.cy()});
    env.ui.handle_pointer_logical(release, {r.cx(), r.cy()});
    CHECK_EQ(tabs.selected(), 0);
}

TEST("forms: text view scrolls with Up/Down, then lets focus move on; short text is not focusable") {
    Env env;
    auto &col = env.ui.root().add<Column>(10);
    auto &above = col.add<Button>("Above", [](Ui &) {});
    std::string longtext;
    for (int i = 0; i < 60; ++i) longtext += "Line of the licence text number " + std::to_string(i) + ". ";
    auto &tv = col.add<TextView>(longtext);
    tv.set_size(600, 200);
    auto &shorttv = col.add<TextView>("Short.");
    shorttv.set_size(600, 200);
    auto &below = col.add<Button>("Below", [](Ui &) {});
    env.ui.focus(&above);
    env.frame();
    CHECK(tv.max_scroll() > 400);
    CHECK(tv.focusable());
    CHECK(!shorttv.focusable());
    env.press(Action::Down);
    CHECK(env.ui.is_focused(&tv));
    int presses = 0;
    while (env.press(Action::Down) && env.ui.is_focused(&tv) && presses < 100) ++presses;
    CHECK(presses > 3);
    CHECK(std::fabs(tv.scroll() - tv.max_scroll()) < 0.01f);
    CHECK(env.ui.is_focused(&below));  // the press at the end moved focus past the short text
    env.press(Action::Up);
    CHECK(env.ui.is_focused(&tv));
    CHECK(env.press(Action::Up));
    CHECK(tv.scroll() < tv.max_scroll());
}

TEST("qr code: encodes text with finder patterns; too much text is an error, not a crash") {
    QrCode qr("https://example.org/activate?code=WNK-1234");
    CHECK(qr.error().empty());
    CHECK(qr.modules() >= 25);
    CHECK_EQ((qr.modules() - 17) % 4, 0);  // 4 * version + 17
    // The finder pattern: a dark 7x7 ring with a dark 3x3 centre, at three corners.
    for (auto [ox, oy] : {std::pair{0, 0}, std::pair{qr.modules() - 7, 0}, std::pair{0, qr.modules() - 7}}) {
        CHECK(qr.dark(ox, oy) && qr.dark(ox + 6, oy + 6) && qr.dark(ox + 3, oy + 3));
        CHECK(!qr.dark(ox + 1, oy + 1) && !qr.dark(ox + 5, oy + 1));
    }
    QrCode big(std::string(8000, 'x'));
    CHECK(!big.error().empty());
    CHECK_EQ(big.modules(), 0);
    Env env;
    auto &v = env.ui.root().add<QrCode>("https://example.org");
    v.set_align(Align::Start, Align::Start);
    env.frame();
    CHECK(env.r.fills.size() > 20);  // runs of dark modules plus the light background
}

TEST("accessibility: widgets describe themselves; password fields never say their content") {
    Button b("Play", [](Ui &) {});
    CHECK_EQ(b.accessible_text(), std::string("Play, button"));
    b.set_enabled(false);
    CHECK_EQ(b.accessible_text(), std::string("Play, button, unavailable"));
    b.accessible_label = "Play from the start";
    CHECK_EQ(b.accessible_text(), std::string("Play from the start"));
    TextField name("User name");
    name.set_text("ana");
    CHECK_EQ(name.accessible_text(), std::string("User name, text field, ana"));
    TextField pw("Password");
    pw.set_password(true).set_text("hunter2");
    CHECK_EQ(pw.accessible_text(), std::string("Password, text field, password, 7 characters"));
    CHECK(pw.accessible_text().find("hunter2") == std::string::npos);
    Toggle t("Subtitles", true);
    CHECK_EQ(t.accessible_text(), std::string("Subtitles, on"));
    Slider s("Volume", 0, 10, 1, 4);
    CHECK_EQ(s.accessible_text(), std::string("Volume, 4"));
    Choice c("Quality", {"Auto", "1080p"}, 1);
    CHECK_EQ(c.accessible_text(), std::string("Quality, 1080p"));
    ValueRow v("Server", "nas.local");
    v.set_description("Where your library is");
    CHECK_EQ(v.accessible_text(), std::string("Server, nas.local, Where your library is"));
    View plain;
    CHECK(plain.accessible_text().empty());
}

TEST("text field: pointer hover does not end editing; a click elsewhere does") {
    Env env;
    auto &col = env.ui.root().add<Column>(10);
    auto &field = col.add<TextField>("Search");
    auto &below = col.add<Button>("Below", [](Ui &) {});
    env.ui.focus(&field);
    env.press(Action::Confirm);
    REQUIRE(field.editing());
    env.frame();
    const Rect r = below.screen_rect();
    input::PointerEvent move;
    move.type = input::PointerEvent::Type::Move;
    env.ui.handle_pointer_logical(move, {r.cx(), r.cy()});  // on the way to LG's keyboard
    CHECK(field.editing());
    CHECK(env.ui.is_focused(&field));
    input::PointerEvent press = move, release = move;
    press.type = input::PointerEvent::Type::Press;
    release.type = input::PointerEvent::Type::Release;
    env.ui.handle_pointer_logical(press, {r.cx(), r.cy()});
    env.ui.handle_pointer_logical(release, {r.cx(), r.cy()});
    CHECK(!field.editing());
    CHECK(env.ui.is_focused(&below));
}
