// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who is signed in and what proves it, kept between launches.
//
// Credentials are provider-neutral: a password login (Jellyfin) fills the user fields and an access
// token; OAuth adds a refresh token and an expiry; an API-key app sets only the access token; a local
// app without accounts does not use this at all. Provider-specific values go in `extra`.
//
// CredentialStore keeps each account's credentials as one sealed secret (storage::SecretStore:
// keymanager3 AES-GCM where the TV has it, else a labelled plain envelope in the app's DB8 kind) and a
// non-secret index of accounts (names, servers, which one was used last) in the key/value store, so a
// "choose account" screen can be drawn without unsealing anything. Every call blocks (Luna round
// trips): run them on a worker thread.
//
// Nothing here formats a token into a log line. describe() is the form to log.
#pragma once

#include "wnk/storage/kv.hpp"
#include "wnk/storage/secret.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wnk::auth {

struct Credentials {
    // The app's name for this sign-in, unique per account (e.g. server URL + user id). Any text;
    // stored under a hash of it.
    std::string account;
    std::string user_id;
    std::string user_name;
    std::string server;         // base URL or realm, if the provider has one
    std::string access_token;   // secret
    std::string refresh_token;  // secret; empty when the provider cannot refresh
    int64_t expires_at = 0;     // unix seconds; 0 when the token has no known expiry
    std::map<std::string, std::string> extra;  // provider fields; stored sealed like the tokens

    bool empty() const { return access_token.empty(); }
    // True when the token expires within `margin_s` of `now` (never for expires_at == 0).
    bool expires_within(int64_t now, int64_t margin_s) const { return expires_at != 0 && now + margin_s >= expires_at; }
};

// "user 'name' on server (expires in 3540 s)": for logs, never contains a token.
std::string describe(const Credentials &c, int64_t now);

// JSON form stored in the sealed secret. parse returns false for anything else (wrong version,
// missing account or token).
std::string to_json(const Credentials &c);
bool from_json(std::string_view text, Credentials *out);

// Non-secret summary of a stored account, for an account list.
struct AccountInfo {
    std::string account, user_name, server;
};

class CredentialStore {
 public:
    // `prefix` namespaces the keys ("auth" → "auth.index", secret "auth.<hash>"); at most 16 characters
    // of [a-z0-9_-] (key/value store key rules).
    CredentialStore(storage::SecretStore &secrets, storage::KeyValueStore &kv, std::string prefix = "auth");

    // Saves (replaces) the credentials of c.account and makes it the last used account.
    storage::SecretStatus save(const Credentials &c);
    // NotFound if never saved; Locked if sealed and keymanager3 is unusable right now (kept intact).
    storage::SecretStatus load(const std::string &account, Credentials *out);
    // Removes the secret and the index entry. Ok also when there was nothing to remove.
    storage::SecretStatus remove(const std::string &account);

    // Most recently used first.
    std::vector<AccountInfo> accounts();
    // The account to restore at launch, or "" when none.
    std::string last_account();

    storage::Protection protection() { return secrets_.protection(); }

 private:
    std::string secret_name(const std::string &account) const;
    bool update_index(const Credentials *add, const std::string *drop);

    storage::SecretStore &secrets_;
    storage::KeyValueStore &kv_;
    std::string prefix_;
};

// A random identifier for this installation of the app (32 hex digits), created on first use and
// kept in `kv` under "<prefix>.installation". Providers that want a device id (Jellyfin's DeviceId)
// get this instead of anything derived from the TV's hardware. Empty when the store fails.
std::string installation_id(storage::KeyValueStore &kv, const std::string &prefix = "auth");

}  // namespace wnk::auth
