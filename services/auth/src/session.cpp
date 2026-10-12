// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/auth/session.hpp"

#include "wnk/log.hpp"

#include <ctime>

namespace wnk::auth {

namespace {
constexpr const char *kCat = "auth";

net::HttpResponse unauthorized(const std::string &detail, net::NetError error = net::NetError::None) {
    net::HttpResponse r;
    r.error = error;
    r.status = error == net::NetError::None ? 401 : 0;
    r.detail = detail;
    return r;
}
}  // namespace

const char *session_state_name(SessionState s) {
    switch (s) {
        case SessionState::SignedOut: return "SignedOut";
        case SessionState::Restoring: return "Restoring";
        case SessionState::SignedIn: return "SignedIn";
        case SessionState::Refreshing: return "Refreshing";
        case SessionState::SigningOut: return "SigningOut";
    }
    return "?";
}

AuthResult AuthResult::ok(Credentials c) {
    AuthResult r;
    r.kind = Kind::Ok;
    r.credentials = std::move(c);
    return r;
}

AuthResult AuthResult::rejected(std::string message) {
    AuthResult r;
    r.kind = Kind::Rejected;
    r.message = std::move(message);
    return r;
}

AuthResult AuthResult::failed(std::string message, net::NetError error) {
    AuthResult r;
    r.kind = Kind::Failed;
    r.message = std::move(message);
    r.net_error = error;
    return r;
}

AuthResult Authenticator::refresh(const Credentials &, const async::CancellationToken &) {
    return AuthResult::rejected("these credentials cannot be refreshed");
}

void Authenticator::revoke(const Credentials &, const async::CancellationToken &) {}

Session::Session(std::shared_ptr<CredentialStore> store, std::shared_ptr<Authenticator> authenticator,
                 async::WorkerPool &pool, async::MainDispatcher &main, SessionOptions options)
    : store_(std::move(store)), auth_(std::move(authenticator)), pool_(pool), main_(main), options_(std::move(options)) {}

Session::~Session() { work_.cancel(); }

int64_t Session::now() const { return options_.now ? options_.now() : static_cast<int64_t>(std::time(nullptr)); }

void Session::set_state(SessionState s, const std::string &why) {
    if (s == state_) return;
    const SessionState from = state_;
    state_ = s;
    WNK_LOGI(kCat, "session %s -> %s%s%s", session_state_name(from), session_state_name(s), why.empty() ? "" : ": ",
             why.c_str());
    if (listener_) listener_(from, s, why);
}

// ---- restore, sign in, sign out ---------------------------------------------------------------------

void Session::restore(std::function<void(SessionState)> done) {
    if (state_ != SessionState::SignedOut || store_ == nullptr) {
        if (done) done(signed_in() ? SessionState::SignedIn : state_);
        return;
    }
    set_state(SessionState::Restoring);
    struct Loaded {
        storage::SecretStatus status = storage::SecretStatus::NotFound;
        Credentials creds;
    };
    async::run_then_post<Loaded>(
        pool_, main_, life_.weak(), work_.token(),
        [store = store_](const async::CancellationToken &) {
            Loaded l;
            const std::string account = store->last_account();
            if (!account.empty()) l.status = store->load(account, &l.creds);
            return l;
        },
        [this, done = std::move(done)](Loaded l) {
            if (state_ == SessionState::Restoring) {
                if (l.status == storage::SecretStatus::Ok) {
                    creds_ = std::move(l.creds);
                    ++generation_;
                    WNK_LOGI(kCat, "restored %s", describe(creds_, now()).c_str());
                    set_state(SessionState::SignedIn);
                    if (creds_.expires_within(now(), options_.refresh_margin_s) && auth_->can_refresh(creds_)) {
                        start_refresh("the token expired while the app was closed");
                    }
                } else if (l.status == storage::SecretStatus::NotFound) {
                    set_state(SessionState::SignedOut, "no saved account");
                } else if (l.status == storage::SecretStatus::Locked) {
                    set_state(SessionState::SignedOut, "the saved credentials are locked (secure storage unavailable)");
                } else {
                    set_state(SessionState::SignedOut, std::string("the saved credentials could not be read (") +
                                                           storage::secret_status_name(l.status) + ")");
                }
            }
            if (done) done(signed_in() ? SessionState::SignedIn : state_);
        });
}

void Session::sign_in(Credentials c, bool remember, std::function<void(storage::SecretStatus)> saved) {
    const bool was_signing_out = state_ == SessionState::SigningOut;
    creds_ = std::move(c);
    ++generation_;  // a refresh or sign-out in flight no longer applies
    WNK_LOGI(kCat, "signed in: %s", describe(creds_, now()).c_str());
    set_state(SessionState::SignedIn);
    // Requests that waited for a refresh go out with the new credentials.
    std::deque<Pending> waiting;
    waiting.swap(waiting_);
    for (Pending &p : waiting) {
        if (!dispatch(p) && p.owner.alive() && !p.token.cancelled()) p.on_done(unauthorized("not sent"));
    }
    if (!remember || store_ == nullptr) {
        if (saved) saved(store_ == nullptr ? storage::SecretStatus::Unavailable : storage::SecretStatus::Ok);
        return;
    }
    if (was_signing_out) {
        // The sign-out's worker may still be removing the stored copy (possibly of this account):
        // save once it has finished.
        deferred_save_ = std::make_unique<DeferredSave>(DeferredSave{creds_, std::move(saved)});
        return;
    }
    save(creds_, std::move(saved));
}

void Session::save(Credentials c, std::function<void(storage::SecretStatus)> saved) {
    async::run_then_post<storage::SecretStatus>(
        pool_, main_, life_.weak(), work_.token(),
        [store = store_, c = std::move(c)](const async::CancellationToken &) { return store->save(c); },
        [saved = std::move(saved)](storage::SecretStatus st) {
            if (st != storage::SecretStatus::Ok) WNK_LOGW(kCat, "credentials not saved: %s", storage::secret_status_name(st));
            if (saved) saved(st);
        });
}

void Session::sign_out(std::function<void()> done) {
    if (!signed_in() && state_ != SessionState::Restoring) {
        if (done) done();
        return;
    }
    Credentials old = std::move(creds_);
    creds_ = Credentials{};
    ++generation_;
    set_state(SessionState::SigningOut);
    fail_pending(unauthorized("signed out"), false);
    const uint64_t gen = generation_;
    async::run_then_post<int>(
        pool_, main_, life_.weak(), {},
        [auth = auth_, store = store_, old = std::move(old)](const async::CancellationToken &t) {
            if (!old.empty()) auth->revoke(old, t);  // nothing to revoke when signed out mid-restore
            if (store != nullptr && !old.account.empty()) {
                const storage::SecretStatus st = store->remove(old.account);
                if (st != storage::SecretStatus::Ok) {
                    WNK_LOGW(kCat, "stored credentials not removed: %s", storage::secret_status_name(st));
                }
            }
            return 0;
        },
        [this, gen, done = std::move(done)](int) {
            if (gen == generation_ && state_ == SessionState::SigningOut) set_state(SessionState::SignedOut, "signed out");
            if (deferred_save_) {
                auto d = std::move(deferred_save_);
                if (signed_in() && d->creds.account == creds_.account) save(creds_, std::move(d->saved));
            }
            if (done) done();
        });
}

void Session::forget_locally(const std::string &why) {
    Credentials old = std::move(creds_);
    creds_ = Credentials{};
    ++generation_;
    set_state(SessionState::SignedOut, why);
    fail_pending(unauthorized(why), true);
    if (store_ == nullptr || old.account.empty()) return;
    // The stored copy is no longer accepted either.
    pool_.submit([store = store_, account = old.account](const async::CancellationToken &) { store->remove(account); });
}

// ---- requests ---------------------------------------------------------------------------------------

bool Session::send(net::HttpClient &http, net::HttpRequest req, async::CancellationToken token,
                   async::Lifetime::Weak owner, net::HttpClient::Callback on_done) {
    if (!signed_in()) return false;
    Pending p;
    p.http = &http;
    p.req = std::move(req);
    p.token = std::move(token);
    p.owner = std::move(owner);
    p.on_done = std::move(on_done);
    if (state_ == SessionState::Refreshing) {
        waiting_.push_back(std::move(p));
        return true;
    }
    if (creds_.expires_within(now(), options_.refresh_margin_s) && auth_->can_refresh(creds_)) {
        waiting_.push_back(std::move(p));
        start_refresh("the token expires soon");
        return true;
    }
    return dispatch(std::move(p));
}

bool Session::dispatch(Pending p) {
    net::HttpRequest r = p.req;
    auth_->authorize(&r, creds_);
    const uint64_t gen = generation_;
    net::HttpClient *http = p.http;
    async::CancellationToken token = p.token;
    return http->send(std::move(r), std::move(token), life_.weak(),
                      [this, p = std::move(p), gen](net::HttpResponse resp) mutable {
                          on_response(std::move(p), gen, std::move(resp));
                      });
}

void Session::on_response(Pending p, uint64_t generation, net::HttpResponse r) {
    if (!p.owner.alive() || p.token.cancelled()) return;
    const bool rejected = r.error == net::NetError::None && r.status == 401;
    if (!rejected || p.retried || !signed_in()) {
        p.on_done(std::move(r));
        return;
    }
    p.retried = true;
    p.first_response = std::move(r);
    if (generation != generation_) {
        // The credentials changed while this request was out (a refresh or a new sign-in): try again
        // with the current ones.
        if (state_ == SessionState::Refreshing) {
            waiting_.push_back(std::move(p));
        } else if (!dispatch(p)) {
            p.on_done(std::move(p.first_response));
        }
        return;
    }
    if (state_ == SessionState::Refreshing || auth_->can_refresh(creds_)) {
        waiting_.push_back(std::move(p));
        start_refresh("the server answered 401");
        return;
    }
    if (!options_.sign_out_on_401) {
        p.on_done(std::move(p.first_response));
        return;
    }
    waiting_.push_back(std::move(p));  // delivered by forget_locally with its 401
    forget_locally("the server no longer accepts the credentials");
}

void Session::start_refresh(const std::string &why) {
    if (state_ == SessionState::Refreshing) return;
    set_state(SessionState::Refreshing, why);
    const uint64_t gen = generation_;
    async::run_then_post<AuthResult>(
        pool_, main_, life_.weak(), work_.token(),
        [auth = auth_, c = creds_](const async::CancellationToken &t) { return auth->refresh(c, t); },
        [this, gen](AuthResult result) {
            if (gen != generation_) return;  // signed in or out meanwhile; that path handled waiting_
            refreshed(std::move(result));
        });
}

void Session::refreshed(AuthResult result) {
    switch (result.kind) {
        case AuthResult::Kind::Ok: {
            const std::string account = creds_.account;
            creds_ = std::move(result.credentials);
            if (creds_.account.empty()) creds_.account = account;
            ++generation_;
            WNK_LOGI(kCat, "refreshed: %s", describe(creds_, now()).c_str());
            set_state(SessionState::SignedIn);
            if (store_ != nullptr) save(creds_, {});
            std::deque<Pending> waiting;
            waiting.swap(waiting_);
            for (Pending &p : waiting) {
                if (!dispatch(p) && p.owner.alive() && !p.token.cancelled()) {
                    p.on_done(p.retried ? p.first_response : unauthorized("not sent"));
                }
            }
            break;
        }
        case AuthResult::Kind::Rejected:
            forget_locally("the token could not be refreshed: " + result.message);
            break;
        case AuthResult::Kind::Failed:
            WNK_LOGW(kCat, "refresh failed, keeping the credentials: %s", result.message.c_str());
            set_state(SessionState::SignedIn);
            fail_pending(unauthorized("token refresh failed: " + result.message, result.net_error), true);
            break;
    }
}

void Session::fail_pending(const net::HttpResponse &r, bool use_first) {
    std::deque<Pending> waiting;
    waiting.swap(waiting_);
    for (Pending &p : waiting) {
        if (!p.owner.alive() || p.token.cancelled()) continue;
        p.on_done(use_first && p.retried ? p.first_response : r);
    }
}

}  // namespace wnk::auth
