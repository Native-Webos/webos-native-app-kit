// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/auth/credentials.hpp"

#include "wnk/log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <random>

namespace wnk::auth {

namespace {

constexpr const char *kCat = "auth";
constexpr size_t kMaxAccounts = 16;  // the index is a convenience list, not a database

using nlohmann::json;

std::string str(const json &o, const char *key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int64_t num(const json &o, const char *key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
}

uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string hex(const uint8_t *bytes, size_t n) {
    static const char *kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 15]);
    }
    return out;
}

}  // namespace

std::string describe(const Credentials &c, int64_t now) {
    std::string out = "user '" + (c.user_name.empty() ? c.user_id : c.user_name) + "'";
    if (!c.server.empty()) out += " on " + c.server;
    if (c.expires_at != 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, " (expires in %lld s)", static_cast<long long>(c.expires_at - now));
        out += buf;
    }
    if (!c.refresh_token.empty()) out += ", refreshable";
    return out;
}

std::string to_json(const Credentials &c) {
    json o = {{"v", 1},
              {"account", c.account},
              {"user_id", c.user_id},
              {"user_name", c.user_name},
              {"server", c.server},
              {"access", c.access_token},
              {"refresh", c.refresh_token},
              {"expires", c.expires_at},
              {"extra", c.extra}};
    return o.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool from_json(std::string_view text, Credentials *out) {
    const json o = json::parse(text.begin(), text.end(), nullptr, false);
    if (!o.is_object() || num(o, "v") != 1) return false;
    Credentials c;
    c.account = str(o, "account");
    c.user_id = str(o, "user_id");
    c.user_name = str(o, "user_name");
    c.server = str(o, "server");
    c.access_token = str(o, "access");
    c.refresh_token = str(o, "refresh");
    c.expires_at = num(o, "expires");
    if (const auto it = o.find("extra"); it != o.end() && it->is_object()) {
        for (const auto &[k, v] : it->items()) {
            if (v.is_string()) c.extra[k] = v.get<std::string>();
        }
    }
    if (c.account.empty() || c.access_token.empty()) return false;
    *out = std::move(c);
    return true;
}

CredentialStore::CredentialStore(storage::SecretStore &secrets, storage::KeyValueStore &kv, std::string prefix)
    : secrets_(secrets), kv_(kv), prefix_(std::move(prefix)) {}

std::string CredentialStore::secret_name(const std::string &account) const {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(fnv1a64(account)));
    return prefix_ + "." + buf;
}

storage::SecretStatus CredentialStore::save(const Credentials &c) {
    if (c.account.empty() || c.access_token.empty()) return storage::SecretStatus::InvalidName;
    const storage::SecretStatus st = secrets_.put(secret_name(c.account), to_json(c));
    if (st != storage::SecretStatus::Ok) {
        WNK_LOGW(kCat, "saving %s: %s", describe(c, 0).c_str(), storage::secret_status_name(st));
        return st;
    }
    if (!update_index(&c, nullptr)) WNK_LOGW(kCat, "account index not updated; the credentials are saved");
    return st;
}

storage::SecretStatus CredentialStore::load(const std::string &account, Credentials *out) {
    std::string text;
    const storage::SecretStatus st = secrets_.get(secret_name(account), &text);
    if (st != storage::SecretStatus::Ok) return st;
    Credentials c;
    if (!from_json(text, &c) || c.account != account) {
        WNK_LOGW(kCat, "stored credentials for an account are unreadable; ignoring them");
        return storage::SecretStatus::Error;
    }
    *out = std::move(c);
    return st;
}

storage::SecretStatus CredentialStore::remove(const std::string &account) {
    storage::SecretStatus st = secrets_.remove(secret_name(account));
    if (st == storage::SecretStatus::NotFound) st = storage::SecretStatus::Ok;
    if (!update_index(nullptr, &account)) WNK_LOGW(kCat, "account index not updated after removal");
    return st;
}

// The index: {"v":1,"accounts":[{"a":account,"u":user name,"s":server}, ...]}, most recent first.
// Read-modify-write with the entry's revision, retried when another writer got there first.
bool CredentialStore::update_index(const Credentials *add, const std::string *drop) {
    const std::string key = prefix_ + ".index";
    for (int attempt = 0; attempt < 4; ++attempt) {
        storage::Entry e;
        const storage::Status got = kv_.get(key, &e);
        if (got != storage::Status::Ok && got != storage::Status::NotFound) return false;
        json list = json::array();
        if (got == storage::Status::Ok) {
            const json o = json::parse(e.value, nullptr, false);
            if (o.is_object() && o.contains("accounts") && o["accounts"].is_array()) list = o["accounts"];
        }
        const std::string &name = add != nullptr ? add->account : *drop;
        json kept = json::array();
        if (add != nullptr) kept.push_back({{"a", add->account}, {"u", add->user_name}, {"s", add->server}});
        for (const json &item : list) {
            if (!item.is_object() || str(item, "a").empty() || str(item, "a") == name) continue;
            if (kept.size() < kMaxAccounts) kept.push_back(item);
        }
        const json doc = {{"v", 1}, {"accounts", kept}};
        const uint64_t expect = got == storage::Status::Ok ? e.revision : storage::kMustNotExist;
        const storage::PutResult put = kv_.put(key, doc.dump(-1, ' ', false, json::error_handler_t::replace), expect);
        if (put.status == storage::Status::Ok) return true;
        if (put.status != storage::Status::Conflict) return false;
    }
    return false;
}

std::vector<AccountInfo> CredentialStore::accounts() {
    std::vector<AccountInfo> out;
    storage::Entry e;
    if (kv_.get(prefix_ + ".index", &e) != storage::Status::Ok) return out;
    const json o = json::parse(e.value, nullptr, false);
    if (!o.is_object() || !o.contains("accounts") || !o["accounts"].is_array()) return out;
    for (const json &item : o["accounts"]) {
        if (!item.is_object() || str(item, "a").empty()) continue;
        out.push_back({str(item, "a"), str(item, "u"), str(item, "s")});
    }
    return out;
}

std::string CredentialStore::last_account() {
    const auto list = accounts();
    return list.empty() ? std::string() : list.front().account;
}

std::string installation_id(storage::KeyValueStore &kv, const std::string &prefix) {
    const std::string key = prefix + ".installation";
    for (int attempt = 0; attempt < 3; ++attempt) {
        storage::Entry e;
        const storage::Status got = kv.get(key, &e);
        if (got == storage::Status::Ok && e.value.size() == 32) return e.value;
        if (got != storage::Status::Ok && got != storage::Status::NotFound) return {};
        uint8_t bytes[16];
        std::random_device rd;  // /dev/urandom on Linux
        for (size_t i = 0; i < sizeof bytes; i += 4) {
            const uint32_t r = rd();
            for (size_t k = 0; k < 4; ++k) bytes[i + k] = static_cast<uint8_t>(r >> (8 * k));
        }
        const std::string id = hex(bytes, sizeof bytes);
        const uint64_t expect = got == storage::Status::Ok ? e.revision : storage::kMustNotExist;
        const storage::PutResult put = kv.put(key, id, expect);
        if (put.status == storage::Status::Ok) return id;
        if (put.status != storage::Status::Conflict) return {};
    }
    return {};
}

}  // namespace wnk::auth
