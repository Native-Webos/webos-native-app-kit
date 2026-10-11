// SPDX-License-Identifier: GPL-3.0-or-later
//
// Screens, the navigation stack and modal dialogs.
//
// Navigator keeps a stack of Screens; only the top one is visible and focusable. Covered screens stay
// alive with their focus nodes registered (but unfocusable), so popping back restores exactly the
// node that was focused, including row/column memory in nested scopes. Back pops the stack; at the
// bottom it is left unhandled for the app to decide.
#pragma once

#include "wnk/ui/anim.hpp"
#include "wnk/ui/widgets.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wnk::ui {

class Screen : public View {
 public:
    explicit Screen(std::string title = {});
    const std::string &title() const { return title_; }
    // Called when the screen becomes the top of the stack / stops being it.
    virtual void on_show(Ui &ui);
    virtual void on_hide(Ui &ui);
    // Return true to consume Back (e.g. to close an inline panel) before the Navigator pops.
    virtual bool on_back(Ui &ui);

 private:
    std::string title_;
};

class Navigator : public View {
 public:
    Navigator();
    // Pushes `screen` on top and focuses inside it (its first focusable view, or what it focused before).
    Screen &push(Ui &ui, std::unique_ptr<Screen> screen);
    // Pops the top screen (unless it is the last) and restores focus on the one below.
    bool pop(Ui &ui);
    // Pops everything above the bottom screen.
    void pop_to_root(Ui &ui);
    size_t depth() const { return stack_.size(); }
    Screen *top() const { return stack_.empty() ? nullptr : stack_.back(); }

    std::function<void(Ui &, Screen &)> on_change;  // after a push or pop, with the new top

 protected:
    bool on_action(Ui &ui, const input::ActionEvent &e) override;
    void update(Ui &ui, float dt) override;
    void draw_over(Ui &ui) override;

 private:
    std::vector<Screen *> stack_;  // owned as children
    Tween enter_;
    // A popped screen is drawn fading out above the new top until its tween ends, then retired.
    std::unique_ptr<View> leaving_;
    Tween leave_;
};

// A modal dialog: title, message, a row of buttons. Shown with Ui::show_dialog(); focus is trapped
// inside until dismissed. A button click runs its handler, then dismisses the dialog.
class Dialog : public View {
 public:
    Dialog(std::string title, std::string message = {});
    Button &add_button(std::string text, ClickHandler on_click = {});
    void dismiss(Ui &ui);
    bool back_dismisses = true;
    std::function<void(Ui &)> on_dismiss;
    Column &body() { return *body_; }  // for extra content above the buttons

 protected:
    void layout_content(Ui &ui, const Rect &content) override;
    Size measure_content(Ui &ui, Size available) override;
    bool on_action(Ui &ui, const input::ActionEvent &e) override;
    void update(Ui &ui, float dt) override;
    void draw(Ui &ui) override;

 private:
    friend class Ui;
    Column *panel_;
    Column *body_;
    Row *buttons_;
    Tween appear_;
    bool dismissed_ = false;
};

}  // namespace wnk::ui
