// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/ui/navigation.hpp"

#include <algorithm>

namespace wnk::ui {
namespace {
constexpr double kEnterMs = 220, kLeaveMs = 180, kDialogMs = 160;
constexpr float kEnterSlide = 40, kLeaveSlide = 60;
}  // namespace

// ---- Screen ---------------------------------------------------------------------------------------

Screen::Screen(std::string title) : title_(std::move(title)) {
    set_focus_scope(false, true);
    set_width(Dim::fill());
    set_height(Dim::fill());
}

void Screen::on_show(Ui &) {}
void Screen::on_hide(Ui &) {}
bool Screen::on_back(Ui &) { return false; }

// ---- Navigator ------------------------------------------------------------------------------------

Navigator::Navigator() {
    set_width(Dim::fill());
    set_height(Dim::fill());
    name = "navigator";
}

Screen &Navigator::push(Ui &ui, std::unique_ptr<Screen> screen) {
    Screen *prev = top();
    Screen &s = static_cast<Screen &>(add_child(std::move(screen)));
    stack_.push_back(&s);
    // Focus the new screen while the old one is still focusable, then hide the old one, so focus
    // never falls back to some unrelated node in between.
    ui.focus_within(&s);
    if (prev != nullptr) {
        prev->set_visible(false);
        prev->on_hide(ui);
    }
    enter_.start(0, 1, ui.now_ms(), prev != nullptr ? kEnterMs : 0);
    s.set_opacity(prev != nullptr ? 0 : 1);
    ui.snap_focus_ring();
    s.on_show(ui);
    if (on_change) on_change(ui, s);
    return s;
}

bool Navigator::pop(Ui &ui) {
    if (stack_.size() <= 1) return false;
    Screen *gone = stack_.back();
    stack_.pop_back();
    Screen *now = stack_.back();
    now->set_visible(true);
    now->set_opacity(1);
    now->set_translation({0, 0});
    gone->on_hide(ui);
    // Restores the node `now` remembered (its nodes stayed registered while it was covered).
    ui.focus_within(now);
    if (leaving_) ui.retire(std::move(leaving_));
    leaving_ = take_child(gone);  // drawn fading out, retired when the tween ends
    leave_.start(0, 1, ui.now_ms(), kLeaveMs);
    enter_ = Tween{};
    ui.snap_focus_ring();
    now->on_show(ui);
    if (on_change) on_change(ui, *now);
    return true;
}

void Navigator::pop_to_root(Ui &ui) {
    while (stack_.size() > 1) pop(ui);
}

bool Navigator::on_action(Ui &ui, const input::ActionEvent &e) {
    if (e.action != input::Action::Back || e.phase != input::Phase::Press) return false;
    if (Screen *s = top(); s != nullptr && s->on_back(ui)) return true;
    return pop(ui);
}

void Navigator::update(Ui &ui, float) {
    if (Screen *s = top(); s != nullptr && enter_.duration_ms > 0) {
        const float t = enter_.value(ui.now_ms());
        s->set_opacity(t);
        s->set_translation({0, (1 - t) * kEnterSlide});
        if (enter_.done(ui.now_ms())) enter_ = Tween{};
    }
    if (leaving_ && leave_.done(ui.now_ms())) ui.retire(std::move(leaving_));
}

void Navigator::draw_over(Ui &ui) {
    if (!leaving_) return;
    const float t = leave_.value(ui.now_ms());
    leaving_->set_opacity(1 - t);
    leaving_->set_translation({t * kLeaveSlide, 0});
    leaving_->draw_tree(ui);
}

// ---- Dialog ---------------------------------------------------------------------------------------

Dialog::Dialog(std::string title, std::string message) {
    set_focus_scope(true, true);
    set_width(Dim::fill());
    set_height(Dim::fill());
    name = "dialog";
    panel_ = &add<Column>(28);
    panel_->set_width(Dim::fixed(920));
    panel_->set_align(Align::Center, Align::Center);
    panel_->set_padding(Insets::all(56));
    panel_->add<Label>(std::move(title), TextRole::Title).set_max_lines(2);
    if (!message.empty()) panel_->add<Label>(std::move(message)).set_muted(true).set_max_lines(6);
    body_ = &panel_->add<Column>(16);
    buttons_ = &panel_->add<Row>(24);
    buttons_->set_padding({0, 16, 0, 0});
}

Button &Dialog::add_button(std::string text, ClickHandler on_click) {
    return buttons_->add<Button>(std::move(text), [this, on_click](Ui &ui) {
        // Dismiss first: the handler may open another dialog, which must end up on top.
        dismiss(ui);
        if (on_click) on_click(ui);
    });
}

void Dialog::dismiss(Ui &ui) {
    if (dismissed_) return;
    dismissed_ = true;
    auto cb = on_dismiss;
    ui.dismiss_dialog(this);
    if (cb) cb(ui);
}

Size Dialog::measure_content(Ui &, Size available) { return available; }

void Dialog::layout_content(Ui &ui, const Rect &content) { panel_->layout(ui, place(ui, *panel_, content)); }

bool Dialog::on_action(Ui &ui, const input::ActionEvent &e) {
    if (e.action != input::Action::Back) return false;
    if (e.phase == input::Phase::Press && back_dismisses) dismiss(ui);
    return true;  // Back never reaches the screen underneath while a dialog is open
}

void Dialog::update(Ui &ui, float) {
    if (appear_.duration_ms == 0) appear_.start(0, 1, ui.now_ms(), kDialogMs);
    const float t = appear_.value(ui.now_ms());
    panel_->set_opacity(t);
    panel_->set_translation({0, (1 - t) * 30});
}

void Dialog::draw(Ui &ui) {
    const float t = appear_.duration_ms == 0 ? 0 : appear_.value(ui.now_ms());
    const Theme &th = ui.theme();
    ui.renderer().fill_rect(frame(), ui.fade(th.scrim.with_alpha(th.scrim.a * t)));
    Rect panel = panel_->frame();
    panel.x += panel_->translation().x;
    panel.y += panel_->translation().y;
    ui.renderer().fill_rect(panel, ui.fade(th.surface.with_alpha(t)), 24);
}

}  // namespace wnk::ui
