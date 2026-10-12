// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/platform/luna.hpp"

#include "wnk/platform/process.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <type_traits>

#include <dlfcn.h>

namespace wnk::platform {

const char *luna_status_name(LunaStatus s) {
    switch (s) {
        case LunaStatus::Ok: return "ok";
        case LunaStatus::Unsupported: return "unsupported";
        case LunaStatus::CallFailed: return "call-failed";
        case LunaStatus::Timeout: return "timeout";
    }
    return "?";
}

namespace {

// Layout from libhelpers.h in the NDK sysroot (webos-helpers). Reproduced here so the binary does not
// need the header at build time on the host.
struct LSHandle;
struct LSMessage;
using LSFilterFunc = bool (*)(LSHandle *, LSMessage *, void *);
struct HContext {
    LSFilterFunc callback;
    void *userdata;
    void *unknown;
    int multiple;
    int pub;
    unsigned long ret_token;
};
using HLunaServiceCallFn = int (*)(const char *, const char *, HContext *);
using HLunaServiceMessageFn = const char *(*)(LSMessage *);

struct Helpers {
    HLunaServiceCallFn call = nullptr;
    HLunaServiceMessageFn message = nullptr;
    std::string error;
};

const Helpers &helpers() {
    static Helpers h = [] {
        Helpers out;
        void *lib = dlopen("libhelpers.so.2", RTLD_NOW | RTLD_GLOBAL);
        if (lib == nullptr) {
            const char *e = dlerror();
            out.error = e != nullptr ? e : "dlopen libhelpers.so.2 failed";
            return out;
        }
        out.call = reinterpret_cast<HLunaServiceCallFn>(dlsym(lib, "HLunaServiceCall"));
        out.message = reinterpret_cast<HLunaServiceMessageFn>(dlsym(lib, "HLunaServiceMessage"));
        if (out.call == nullptr || out.message == nullptr) out.error = "libhelpers.so.2 lacks HLunaServiceCall";
        return out;
    }();
    return h;
}

// One call's state. The libhelpers context must stay valid until the reply arrives, so after a timeout
// the state is deliberately leaked rather than freed under a callback that may still run.
struct CallState {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    std::string payload;
};

// libhelpers hands the HContext pointer back as the callback's ctx (commons-c luna-sync relies on the
// same), so the context sits first in a standard-layout box that also points at the call state.
struct CallBox {
    union {
        HContext ctx;
        unsigned char placeholder[128];  // libhelpers may keep private fields past the public ones
    } base;
    CallState *state;
};
static_assert(std::is_standard_layout<CallBox>::value, "CallBox must be standard-layout");

bool on_reply(LSHandle *, LSMessage *reply, void *ctx) {
    auto *state = reinterpret_cast<CallBox *>(ctx)->state;
    const char *text = helpers().message(reply);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->finished) return true;
    state->payload = text != nullptr ? text : "";
    state->finished = true;
    state->cv.notify_all();
    return true;
}

// A subscription's state: shared by the handle and the (leaked) call box, so a reply after close()
// finds it alive and sees `open == false`.
struct SubBox {
    union {
        HContext ctx;
        unsigned char placeholder[128];
    } base;
    std::shared_ptr<LunaSubscription::State> *state;  // heap-held, so the box stays standard-layout
};
static_assert(std::is_standard_layout<SubBox>::value, "SubBox must be standard-layout");

}  // namespace

struct LunaSubscription::State {
    std::mutex mutex;
    bool open = true;
    std::function<void(const std::string &)> on_reply;
};

void LunaSubscription::close() {
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->open = false;
    state_->on_reply = nullptr;
}

bool LunaSubscription::active() const {
    if (!state_) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->open;
}

namespace {
bool on_sub_reply(LSHandle *, LSMessage *reply, void *ctx) {
    LunaSubscription::State &st = **reinterpret_cast<SubBox *>(ctx)->state;
    const char *text = helpers().message(reply);
    std::lock_guard<std::mutex> lock(st.mutex);
    if (st.open && st.on_reply) st.on_reply(text != nullptr ? text : "");
    return true;
}
}  // namespace

std::unique_ptr<LunaSubscription> luna_subscribe(const std::string &uri, const std::string &payload,
                                                 std::function<void(const std::string &)> on_reply, bool public_bus,
                                                 std::string *error) {
    auto set_error = [&](const std::string &e) {
        if (error != nullptr) *error = e;
        return nullptr;
    };
    if (!kIsWebOS) return set_error("not a webOS build");
    const Helpers &h = helpers();
    if (h.call == nullptr) return set_error(h.error);
    auto sub = std::unique_ptr<LunaSubscription>(new LunaSubscription());
    sub->state_ = std::make_shared<LunaSubscription::State>();
    sub->state_->on_reply = std::move(on_reply);
    auto *box = new SubBox();  // never freed: libhelpers may call back for as long as the bus call lives
    std::memset(&box->base, 0, sizeof box->base);
    box->state = new std::shared_ptr<LunaSubscription::State>(sub->state_);
    box->base.ctx.callback = on_sub_reply;
    box->base.ctx.multiple = 1;
    box->base.ctx.pub = public_bus ? 1 : 0;
    if (h.call(uri.c_str(), payload.c_str(), &box->base.ctx) != 0) {
        delete box->state;
        delete box;
        return set_error("HLunaServiceCall returned an error");
    }
    return sub;
}

LunaReply luna_call_sync(const std::string &uri, const std::string &payload, int timeout_ms, bool public_bus) {
    LunaReply r;
    if (!kIsWebOS) {
        r.detail = "not a webOS build";
        return r;
    }
    const Helpers &h = helpers();
    if (h.call == nullptr) {
        r.detail = h.error;
        return r;
    }
    auto *state = new CallState();
    auto *box = new CallBox();
    std::memset(&box->base, 0, sizeof box->base);
    box->state = state;
    box->base.ctx.callback = on_reply;
    box->base.ctx.multiple = 0;
    box->base.ctx.pub = public_bus ? 1 : 0;
    if (h.call(uri.c_str(), payload.c_str(), &box->base.ctx) != 0) {
        delete box;
        delete state;
        r.status = LunaStatus::CallFailed;
        r.detail = "HLunaServiceCall returned an error";
        return r;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    const bool done = state->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return state->finished; });
    if (!done) {
        // Leak box and state: a late reply may still arrive and must find valid memory. Unlock normally,
        // otherwise that late reply would block libhelpers' thread forever.
        lock.unlock();
        r.status = LunaStatus::Timeout;
        r.detail = "no reply within " + std::to_string(timeout_ms) + " ms";
        return r;
    }
    r.status = LunaStatus::Ok;
    r.payload = state->payload;
    lock.unlock();
    delete box;
    delete state;
    return r;
}

}  // namespace wnk::platform
