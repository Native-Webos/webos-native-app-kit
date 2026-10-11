// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/storage/secret.hpp"

#include "wnk/log.hpp"
#include "wnk/platform/luna.hpp"

#include <nlohmann/json.hpp>

namespace wnk::storage {
namespace {

constexpr const char *kCat = "secret";
constexpr const char *kSealed = "keymanager3-aes256-gcm-v1";
constexpr const char *kPlain = "plain-v1";
constexpr int kKeyExists = -10002;

using json = nlohmann::json;

std::optional<json> parse(const std::optional<std::string> &text) {
    if (!text) return std::nullopt;
    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    return j;
}

bool ok(const json &j) { return j.contains("returnValue") && j["returnValue"].is_boolean() && j["returnValue"].get<bool>(); }

std::string describe(const std::optional<json> &j) {
    if (!j) return "no reply";
    std::string s = "errorCode " + (j->contains("errorCode") ? j->at("errorCode").dump() : std::string("?"));
    if (j->contains("errorText") && j->at("errorText").is_string()) s += " " + j->at("errorText").get<std::string>();
    return s;
}

class LunaKeyService : public KeyService {
 public:
    explicit LunaKeyService(int timeout_ms) : timeout_ms_(timeout_ms) {}
    std::optional<std::string> call(const std::string &method, const std::string &payload) override {
        const auto r = platform::luna_call_sync("luna://com.webos.service.keymanager3/" + method, payload, timeout_ms_);
        if (r.status != platform::LunaStatus::Ok) return std::nullopt;
        return r.payload;
    }

 private:
    int timeout_ms_;
};

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::unique_ptr<KeyService> luna_keymanager3(int timeout_ms) { return std::make_unique<LunaKeyService>(timeout_ms); }

const char *protection_name(Protection p) { return p == Protection::Keymanager3 ? "keymanager3" : "plain"; }

const char *secret_status_name(SecretStatus s) {
    switch (s) {
        case SecretStatus::Ok: return "ok";
        case SecretStatus::NotFound: return "not-found";
        case SecretStatus::Locked: return "locked";
        case SecretStatus::InvalidName: return "invalid-name";
        case SecretStatus::Unavailable: return "unavailable";
        case SecretStatus::Error: return "error";
    }
    return "?";
}

std::string base64_encode(const std::string &in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += kAlphabet[v >> 18];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint8_t(in[i]) << 16;
        if (i + 1 < in.size()) v |= uint8_t(in[i + 1]) << 8;
        out += kAlphabet[v >> 18];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < in.size() ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::optional<std::string> base64_decode(const std::string &in) {
    if (in.size() % 4 != 0) return std::nullopt;
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = in[i + k];
            if (c == '=' && i + 4 == in.size() && k >= 2) {
                v[k] = 0;
                ++pad;
            } else {
                if (pad > 0) return std::nullopt;  // data after padding
                v[k] = value(c);
                if (v[k] < 0) return std::nullopt;
            }
        }
        const uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
        out += static_cast<char>(n >> 16);
        if (pad < 2) out += static_cast<char>((n >> 8) & 0xFF);
        if (pad < 1) out += static_cast<char>(n & 0xFF);
    }
    return out;
}

SecretStore::SecretStore(KeyValueStore &kv, std::unique_ptr<KeyService> keys, std::string key_name)
    : kv_(kv), keys_(std::move(keys)), key_name_(std::move(key_name)) {}

std::string SecretStore::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

bool SecretStore::key_ready() {
    if (!keys_) {
        last_error_ = "no keymanager transport";
        return false;
    }
    const json params = {{"type", "AES"},       {"size", 256},           {"mode", {"GCM"}},
                         {"purpose", {"encrypt", "decrypt"}}, {"padding", {"None"}}};
    const auto reply = parse(keys_->call("generateKey", json{{"name", key_name_}, {"params", params}}.dump()));
    if (reply && (ok(*reply) || (reply->contains("errorCode") && reply->at("errorCode") == kKeyExists))) return true;
    last_error_ = "generateKey: " + describe(reply);
    return false;
}

Protection SecretStore::protection() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (probe_ == Probe::Unknown) {
        probe_ = key_ready() ? Probe::Available : Probe::Unavailable;
        if (probe_ == Probe::Available) {
            WNK_LOGI(kCat, "keymanager3 available: secrets are sealed with AES-256-GCM (key %s)", key_name_.c_str());
        } else {
            WNK_LOGW(kCat, "keymanager3 unusable (%s): secrets are stored without protection", last_error_.c_str());
        }
    }
    return probe_ == Probe::Available ? Protection::Keymanager3 : Protection::Plain;
}

void SecretStore::abort(const std::string &handle) { keys_->call("abort", json{{"handle", handle}}.dump()); }

std::optional<std::string> SecretStore::seal(const std::string &plain, std::string *iv) {
    const json params = {{"type", "AES"}, {"mode", {"GCM"}}, {"purpose", {"encrypt"}}, {"padding", {"None"}}, {"mac_length", "128"}};
    const auto begin = parse(keys_->call("begin", json{{"name", key_name_}, {"params", params}}.dump()));
    if (!begin || !ok(*begin) || !begin->contains("handle") || !begin->contains("iv")) {
        last_error_ = "begin(encrypt): " + describe(begin);
        if (begin && begin->contains("handle") && begin->at("handle").is_string()) abort(begin->at("handle").get<std::string>());
        return std::nullopt;
    }
    const std::string handle = begin->at("handle").get<std::string>();
    *iv = begin->at("iv").get<std::string>();
    const auto finish = parse(keys_->call("finish", json{{"handle", handle}, {"data", base64_encode(plain)}}.dump()));
    if (!finish || !ok(*finish) || !finish->contains("output") || !finish->at("output").is_string()) {
        last_error_ = "finish(encrypt): " + describe(finish);
        abort(handle);
        return std::nullopt;
    }
    return finish->at("output").get<std::string>();
}

std::optional<std::string> SecretStore::open_sealed(const std::string &data, const std::string &iv) {
    const json params = {{"type", "AES"},       {"mode", {"GCM"}},   {"purpose", {"decrypt"}},
                         {"padding", {"None"}}, {"mac_length", "128"}, {"iv", iv}};
    const auto begin = parse(keys_->call("begin", json{{"name", key_name_}, {"params", params}}.dump()));
    if (!begin || !ok(*begin) || !begin->contains("handle")) {
        last_error_ = "begin(decrypt): " + describe(begin);
        return std::nullopt;
    }
    const std::string handle = begin->at("handle").get<std::string>();
    const auto finish = parse(keys_->call("finish", json{{"handle", handle}, {"data", data}}.dump()));
    if (!finish || !ok(*finish) || !finish->contains("output") || !finish->at("output").is_string()) {
        // GCM authentication failure (tampered or foreign data) ends here too.
        last_error_ = "finish(decrypt): " + describe(finish);
        abort(handle);
        return std::nullopt;
    }
    return base64_decode(finish->at("output").get<std::string>());
}

SecretStatus SecretStore::put(const std::string &name, const std::string &value) {
    const std::string key = "secret." + name;
    if (name.size() > 56 || !is_valid_key(key)) return SecretStatus::InvalidName;
    const Protection p = protection();
    std::lock_guard<std::mutex> lock(mutex_);
    json env;
    if (p == Protection::Keymanager3) {
        std::string iv;
        const auto data = seal(value, &iv);
        if (!data) {
            probe_ = Probe::Unknown;  // re-probe next time; do not fall back to plain for this write
            WNK_LOGW(kCat, "sealing '%s' failed: %s", name.c_str(), last_error_.c_str());
            return SecretStatus::Error;
        }
        env = {{"format", kSealed}, {"key", key_name_}, {"iv", iv}, {"data", *data}};
    } else {
        // Never replace a sealed secret with an unprotected one.
        Entry existing;
        if (kv_.get(key, &existing) == Status::Ok) {
            const json old = json::parse(existing.value, nullptr, false);
            if (old.is_object() && old.value("format", "") == kSealed) {
                WNK_LOGW(kCat, "refusing to overwrite sealed '%s' without keymanager3", name.c_str());
                return SecretStatus::Locked;
            }
        }
        env = {{"format", kPlain}, {"data", base64_encode(value)}};
    }
    const PutResult r = kv_.put(key, env.dump());
    if (r.status == Status::Ok) return SecretStatus::Ok;
    return r.status == Status::Unavailable ? SecretStatus::Unavailable : SecretStatus::Error;
}

SecretStatus SecretStore::get(const std::string &name, std::string *out) {
    const std::string key = "secret." + name;
    if (name.size() > 56 || !is_valid_key(key)) return SecretStatus::InvalidName;
    Entry e;
    const Status st = kv_.get(key, &e);
    if (st == Status::NotFound) return SecretStatus::NotFound;
    if (st == Status::Unavailable) return SecretStatus::Unavailable;
    if (st != Status::Ok) return SecretStatus::Error;
    const json env = json::parse(e.value, nullptr, false);
    if (!env.is_object()) return SecretStatus::Error;
    const std::string format = env.value("format", "");
    if (format == kPlain) {
        const auto v = base64_decode(env.value("data", ""));
        if (!v) return SecretStatus::Error;
        *out = *v;
        return SecretStatus::Ok;
    }
    if (format != kSealed || env.value("key", "") != key_name_) return SecretStatus::Locked;
    if (protection() != Protection::Keymanager3) return SecretStatus::Locked;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto v = open_sealed(env.value("data", ""), env.value("iv", ""));
    if (!v) {
        WNK_LOGW(kCat, "opening '%s' failed: %s", name.c_str(), last_error_.c_str());
        return SecretStatus::Locked;
    }
    *out = *v;
    return SecretStatus::Ok;
}

SecretStatus SecretStore::remove(const std::string &name) {
    const std::string key = "secret." + name;
    if (name.size() > 56 || !is_valid_key(key)) return SecretStatus::InvalidName;
    const Status st = kv_.remove(key);
    if (st == Status::Ok) return SecretStatus::Ok;
    if (st == Status::NotFound) return SecretStatus::NotFound;
    return st == Status::Unavailable ? SecretStatus::Unavailable : SecretStatus::Error;
}

}  // namespace wnk::storage
