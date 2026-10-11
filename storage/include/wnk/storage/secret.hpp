// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small secrets (access tokens, passwords) for native apps.
//
// Each secret is stored as an envelope in a KeyValueStore (DB8 on the TV: the Developer Mode jail
// has no durable writable filesystem) under "secret.<name>":
//
//   {"format":"keymanager3-aes256-gcm-v1","key":"<key name>","iv":"...","data":"..."}   sealed
//   {"format":"plain-v1","data":"<base64>"}                                            not protected
//
// Sealing uses the firmware's com.webos.service.keymanager3 (AES-256-GCM, authenticated): generateKey
// once (-10002 = already exists), then begin → finish per operation, abort on failure. That service is
// documented for webOS TV 24+, so it is probed at runtime and its absence or a permission denial selects
// the plain envelope; protection() says which one is in effect, so an app can refuse to save a secret
// unprotected. The legacy com.palm.keymanager is not used: AES-CFB authenticates nothing. Protocol and
// policy follow PlxNative's rust-modules/platform/src/keymanager.rs (GPL-3.0-or-later).
//
// Never a silent downgrade: a sealed secret is never overwritten with a plain envelope, and reading a
// sealed secret while keymanager3 is unusable returns Locked (the data stays intact for later).
//
// Apps need "requiredPermissions": ["database.operation", "securitykey.operation"] in appinfo.json.
// Calls block (Luna round trips): use them at startup or from a worker thread.
#pragma once

#include "wnk/storage/kv.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace wnk::storage {

// Transport for com.webos.service.keymanager3; tests inject a fake.
class KeyService {
 public:
    virtual ~KeyService() = default;
    // Calls luna://com.webos.service.keymanager3/<method>. nullopt if the bus call failed or timed out.
    virtual std::optional<std::string> call(const std::string &method, const std::string &payload) = 0;
};

// The production transport (Luna through libhelpers); on non-webOS builds every call fails.
std::unique_ptr<KeyService> luna_keymanager3(int timeout_ms = 4000);

enum class Protection { Keymanager3, Plain };
const char *protection_name(Protection p);

enum class SecretStatus { Ok, NotFound, Locked, InvalidName, Unavailable, Error };
const char *secret_status_name(SecretStatus s);

class SecretStore {
 public:
    // `keys` may be null (no keymanager: plain only). `key_name` names the AES key in keymanager3,
    // e.g. "<app id>.secrets.v1".
    SecretStore(KeyValueStore &kv, std::unique_ptr<KeyService> keys, std::string key_name);

    // Probes keymanager3 once (cached; re-probed after a failure on use).
    Protection protection();
    // Names follow KeyValueStore key rules, at most 56 characters.
    SecretStatus put(const std::string &name, const std::string &value);
    SecretStatus get(const std::string &name, std::string *out);
    SecretStatus remove(const std::string &name);
    // Why the last keymanager3 call failed (for logs).
    std::string last_error() const;

 private:
    enum class Probe { Unknown, Available, Unavailable };
    bool key_ready();
    std::optional<std::string> seal(const std::string &plain, std::string *iv);
    std::optional<std::string> open_sealed(const std::string &data, const std::string &iv);
    void abort(const std::string &handle);

    KeyValueStore &kv_;
    std::unique_ptr<KeyService> keys_;
    std::string key_name_;
    Probe probe_ = Probe::Unknown;
    std::string last_error_;
    mutable std::mutex mutex_;
};

// Standard base64 (RFC 4648), used for envelopes. decode returns nullopt on malformed input.
std::string base64_encode(const std::string &bytes);
std::optional<std::string> base64_decode(const std::string &text);

}  // namespace wnk::storage
