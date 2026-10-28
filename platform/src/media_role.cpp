// SPDX-License-Identifier: GPL-3.0-or-later
//
// Follows client-webos's ndl::ensure_media_role and ls2::may_register (punktfunk, the bus prefix, the
// template path and the register-then-unregister probe).
#include "wnk/platform/media_role.hpp"

#include <fstream>
#include <mutex>
#include <sstream>

#include <dlfcn.h>
#include <unistd.h>

namespace wnk::platform {
namespace {

constexpr const char *kTemplate = "/usr/share/rolegen/templates/NDK.json";
constexpr const char *kPlayerBus = "com.webos.media.smp.";

// luna-service2's LSError (lunaservice.h), declared here: the library is dlopened.
struct LsError {
    int error_code;
    char *message;
    const char *file;
    int line;
    const char *func;
    void *padding;
    unsigned long magic;
};

struct Probe {
    bool ok = true;
    std::string why;
};

Probe probe() {
    std::ifstream f(kTemplate);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    if (!media_role_template_names_player_bus(ss.str())) return {};
    void *lib = dlopen("libluna-service2.so.3", RTLD_NOW | RTLD_GLOBAL);
    if (lib == nullptr) return {};  // no bus library: no answer, not a refusal
    auto init = reinterpret_cast<bool (*)(LsError *)>(dlsym(lib, "LSErrorInit"));
    auto free_error = reinterpret_cast<void (*)(LsError *)>(dlsym(lib, "LSErrorFree"));
    auto reg = reinterpret_cast<bool (*)(const char *, void **, LsError *)>(dlsym(lib, "LSRegister"));
    auto unreg = reinterpret_cast<bool (*)(void *, LsError *)>(dlsym(lib, "LSUnregister"));
    if (init == nullptr || free_error == nullptr || reg == nullptr || unreg == nullptr) return {};
    const std::string name = std::string(kPlayerBus) + "wnk" + std::to_string(getpid());
    LsError err{};
    init(&err);
    void *handle = nullptr;
    Probe p;
    if (reg(name.c_str(), &handle, &err) && handle != nullptr) {
        unreg(handle, &err);
    } else {
        p.ok = false;
        p.why = std::string("this TV's software update left the app's permissions out of date (") +
                (err.message != nullptr ? err.message : ("code " + std::to_string(err.error_code))) +
                "): reinstall the app";
    }
    free_error(&err);
    return p;
}

}  // namespace

bool media_role_template_names_player_bus(const std::string &template_text) {
    return template_text.find(kPlayerBus) != std::string::npos;
}

bool media_role_ok(std::string *why) {
    static std::once_flag once;
    static Probe result;
    std::call_once(once, [] { result = probe(); });
    if (!result.ok && why != nullptr) *why = result.why;
    return result.ok;
}

}  // namespace wnk::platform
