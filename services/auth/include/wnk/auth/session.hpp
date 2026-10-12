// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session: the signed-in state of an app, on the main thread.
//
//   SignedOut ─restore()──► Restoring ──(stored account)──► SignedIn ◄──┐
//       ▲  ◄─────────────────────┘ (none, locked)              │        │
//       │                                    expiring or 401 ──┤   ok   │
//       │                                                      ▼        │
//       ├──────────(provider rejects the refresh)────────── Refreshing ─┘
//       └───────── SigningOut ◄──sign_out()── (any signed-in state)
//
// The app runs its own login flow (password form, device code, OAuth in a companion app) and hands the
// result to sign_in(); the session does not know what the flow was. What it does know how to do is
// provider-neutral and easy to get wrong in every app:
//
//   * send() adds the credentials to a request (Authenticator::authorize) and sends it through an
//     HttpClient. A token about to expire is refreshed first. A 401 refreshes once and the request is
//     retried with the new token; requests that arrive or fail while a refresh runs wait for that one
//     refresh instead of starting their own. A refresh the provider rejects signs out.
//   * sign_out() forgets the credentials in memory at once, asks the provider to end the session
//     (best effort, on a worker) and removes the stored copy.
//   * Credentials are saved through CredentialStore on a worker; the main thread never blocks on Luna.
//
// Threading: every public method is a main-thread call and every callback runs on the main thread.
// Authenticator::refresh and revoke run on the worker pool with copies of the credentials.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/auth/credentials.hpp"
#include "wnk/net/http.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace wnk::auth {

enum class SessionState : uint8_t { SignedOut, Restoring, SignedIn, Refreshing, SigningOut };
const char *session_state_name(SessionState s);

struct AuthResult {
    enum class Kind : uint8_t {
        Ok,        // `credentials` replace the current ones
        Rejected,  // the provider refused (revoked token, changed password): sign out
        Failed,    // could not ask (network, server error): keep the credentials, try again later
    };
    Kind kind = Kind::Failed;
    Credentials credentials;
    net::NetError net_error = net::NetError::None;  // Failed: why, when it was the network
    std::string message;  // one line for logs and the screen; never contains a secret

    static AuthResult ok(Credentials c);
    static AuthResult rejected(std::string message);
    static AuthResult failed(std::string message, net::NetError error = net::NetError::None);
};

// The provider-specific part, implemented by the app.
class Authenticator {
 public:
    virtual ~Authenticator() = default;
    // Main thread: put the credentials on a request (an Authorization header, a token header).
    virtual void authorize(net::HttpRequest *req, const Credentials &c) const = 0;
    // Whether refresh() can do anything with these credentials (default: a refresh token is present).
    virtual bool can_refresh(const Credentials &c) const { return !c.refresh_token.empty(); }
    // Worker thread: new credentials from `current`. The default rejects (nothing to refresh with).
    virtual AuthResult refresh(const Credentials &current, const async::CancellationToken &token);
    // Worker thread, during sign_out: end the session at the provider (Jellyfin POST /Sessions/Logout,
    // OAuth token revocation). Best effort; keep its timeout short. The default does nothing.
    virtual void revoke(const Credentials &current, const async::CancellationToken &token);
};

struct SessionOptions {
    int64_t refresh_margin_s = 60;  // refresh before sending when the token expires within this
    // A 401 for credentials that cannot be refreshed means the provider no longer accepts them (a
    // revoked token, a changed password): sign out. Turn off for APIs that answer 401 for other reasons.
    bool sign_out_on_401 = true;
    std::function<int64_t()> now;   // unix seconds; the system clock when empty (tests inject one)
};

class Session {
 public:
    // `store` may be null: credentials then live in memory only (sign_in's `remember` is ignored).
    Session(std::shared_ptr<CredentialStore> store, std::shared_ptr<Authenticator> authenticator,
            async::WorkerPool &pool, async::MainDispatcher &main, SessionOptions options = {});
    ~Session();  // cancels a refresh or store operation in flight; pending requests are dropped
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Every state change, with a reason for SignedOut ("signed out", "the server rejected the token").
    using Listener = std::function<void(SessionState from, SessionState to, const std::string &why)>;
    void set_listener(Listener listener) { listener_ = std::move(listener); }

    SessionState state() const { return state_; }
    bool signed_in() const { return state_ == SessionState::SignedIn || state_ == SessionState::Refreshing; }
    // The current credentials; empty when signed out.
    const Credentials &credentials() const { return creds_; }

    // At launch: loads the last used account. `done` receives the resulting state (SignedIn or
    // SignedOut). Locked credentials (keymanager3 unusable right now) are kept in the store and the
    // session stays signed out with that reason.
    void restore(std::function<void(SessionState)> done = {});

    // After the app's login flow: these credentials are current from now on. With `remember` they are
    // saved (on a worker; `saved` receives the status, e.g. to warn when protection() is Plain).
    void sign_in(Credentials c, bool remember = true, std::function<void(storage::SecretStatus)> saved = {});

    // Forgets the credentials now; revokes and removes the stored copy on a worker, then `done`.
    // Requests waiting for a refresh complete with status 401.
    void sign_out(std::function<void()> done = {});

    // Sends `req` with the credentials, as HttpClient::send does (on_done on the main thread unless
    // `owner` died or `token` was cancelled). Returns false, without calling on_done, when signed out
    // or the client refused the request.
    bool send(net::HttpClient &http, net::HttpRequest req, async::CancellationToken token, async::Lifetime::Weak owner,
              net::HttpClient::Callback on_done);

 private:
    struct Pending {
        net::HttpClient *http = nullptr;
        net::HttpRequest req;
        async::CancellationToken token;
        async::Lifetime::Weak owner;
        net::HttpClient::Callback on_done;
        bool retried = false;              // already sent once and answered 401
        net::HttpResponse first_response;  // that 401, delivered if the refresh fails
    };
    int64_t now() const;
    void set_state(SessionState s, const std::string &why = {});
    bool dispatch(Pending p);
    void on_response(Pending p, uint64_t generation, net::HttpResponse r);
    void start_refresh(const std::string &why);
    void refreshed(AuthResult result);
    void fail_pending(const net::HttpResponse &r, bool use_first);
    void save(Credentials c, std::function<void(storage::SecretStatus)> saved);
    void forget_locally(const std::string &why);

    struct DeferredSave {
        Credentials creds;
        std::function<void(storage::SecretStatus)> saved;
    };

    std::shared_ptr<CredentialStore> store_;
    std::shared_ptr<Authenticator> auth_;
    async::WorkerPool &pool_;
    async::MainDispatcher &main_;
    SessionOptions options_;
    Listener listener_;
    SessionState state_ = SessionState::SignedOut;
    Credentials creds_;
    uint64_t generation_ = 0;  // bumped whenever creds_ change; a 401 for an older one just retries
    std::deque<Pending> waiting_;  // for the refresh in flight
    std::unique_ptr<DeferredSave> deferred_save_;  // sign_in during SigningOut: saved after the removal
    async::CancellationSource work_;  // refresh and store operations
    async::Lifetime life_;
};

}  // namespace wnk::auth
