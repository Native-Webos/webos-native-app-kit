// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keeping the TV's screensaver away while something on screen needs watching.
//
// The TV asks the foreground app before starting its screensaver (tvpower registerScreenSaverRequest;
// SDL-webOS answers it from SDL's screensaver state). An app that always refuses keeps a static menu
// on the panel for hours, a burn-in risk on OLED sets, so the shell allows the screensaver by default
// and the app holds a WakeLock only while video plays or a game runs.
//
// SDL-free: the shell supplies the function that applies the state. Main thread only.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace wnk::app {

class WakeLocks;

// Held while the screensaver must not start; released by release() or destruction. Move-only.
// A lock that outlives the app shell is harmless.
class WakeLock {
 public:
    WakeLock() = default;
    WakeLock(WakeLock &&other) noexcept;
    WakeLock &operator=(WakeLock &&other) noexcept;
    WakeLock(const WakeLock &) = delete;
    WakeLock &operator=(const WakeLock &) = delete;
    ~WakeLock() { release(); }

    bool held() const { return owner_ != nullptr; }
    void release();

 private:
    friend class WakeLocks;
    struct Shared;
    WakeLock(std::shared_ptr<Shared> owner, std::string reason);
    std::shared_ptr<Shared> owner_;
    std::string reason_;
};

// Counts the held locks and applies "block" / "allow" when the count leaves or returns to zero.
class WakeLocks {
 public:
    // apply(block, reason) is called on every change of state. With allow_screensaver false the
    // screensaver is blocked for the whole run and locks change nothing.
    WakeLocks(bool allow_screensaver, std::function<void(bool block, const std::string &reason)> apply);
    ~WakeLocks();
    WakeLocks(const WakeLocks &) = delete;
    WakeLocks &operator=(const WakeLocks &) = delete;

    WakeLock acquire(std::string reason);
    int held() const;
    bool blocking() const;
    // Stop applying (the shell calls this before SDL_Quit); locks released later do nothing.
    void detach();

 private:
    std::shared_ptr<WakeLock::Shared> shared_;
};

}  // namespace wnk::app
