// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/app/wake_lock.hpp"

#include <utility>

namespace wnk::app {

struct WakeLock::Shared {
    bool allow = true;
    int count = 0;
    std::function<void(bool, const std::string &)> apply;
};

WakeLock::WakeLock(std::shared_ptr<Shared> owner, std::string reason) : owner_(std::move(owner)), reason_(std::move(reason)) {}

WakeLock::WakeLock(WakeLock &&other) noexcept : owner_(std::move(other.owner_)), reason_(std::move(other.reason_)) {}

WakeLock &WakeLock::operator=(WakeLock &&other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::move(other.owner_);
        reason_ = std::move(other.reason_);
    }
    return *this;
}

void WakeLock::release() {
    if (!owner_) return;
    auto owner = std::move(owner_);
    owner_.reset();
    if (--owner->count == 0 && owner->allow && owner->apply) owner->apply(false, reason_);
}

WakeLocks::WakeLocks(bool allow_screensaver, std::function<void(bool, const std::string &)> apply)
    : shared_(std::make_shared<WakeLock::Shared>()) {
    shared_->allow = allow_screensaver;
    shared_->apply = std::move(apply);
    if (shared_->apply) shared_->apply(!allow_screensaver, allow_screensaver ? "default" : "AppConfig::allow_screensaver is false");
}

WakeLocks::~WakeLocks() { detach(); }

WakeLock WakeLocks::acquire(std::string reason) {
    if (++shared_->count == 1 && shared_->allow && shared_->apply) shared_->apply(true, reason);
    return WakeLock(shared_, std::move(reason));
}

int WakeLocks::held() const { return shared_->count; }

bool WakeLocks::blocking() const { return !shared_->allow || shared_->count > 0; }

void WakeLocks::detach() { shared_->apply = nullptr; }

}  // namespace wnk::app
