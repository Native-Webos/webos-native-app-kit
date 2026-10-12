// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/auth: credentials stored through SecretStore (plain envelopes in a FileStore here), and the
// Session against a fake server that accepts one token at a time and a fake provider whose refresh
// can succeed, be rejected or fail.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/auth/session.hpp"
#include "wnk/log.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace wnk;
using namespace wnk::auth;
using namespace std::chrono_literals;

namespace {

// Accepts "Bearer <valid>" and answers 401 to anything else; records the tokens it saw.
struct FakeServer final : net::HttpTransport {
    std::mutex m;
    std::string valid = "t1";
    std::vector<std::string> seen;
    std::chrono::milliseconds delay{0};

    net::HttpResponse perform(const net::HttpRequest &req, const async::CancellationToken &) override {
        std::this_thread::sleep_for(delay);
        std::string auth;
        for (const auto &[k, v] : req.headers) {
            if (k == "Authorization") auth = v;
        }
        std::lock_guard<std::mutex> lock(m);
        seen.push_back(auth);
        net::HttpResponse r;
        r.status = auth == "Bearer " + valid ? 200 : 401;
        r.body = req.url;
        return r;
    }
    std::string describe() const override { return "fake"; }
    size_t count(const std::string &token) {
        std::lock_guard<std::mutex> lock(m);
        size_t n = 0;
        for (const auto &s : seen) n += s == "Bearer " + token;
        return n;
    }
};

struct FakeProvider final : Authenticator {
    enum class Mode { Ok, Rejected, Failed } mode = Mode::Ok;
    std::atomic<int> refreshes{0}, revokes{0};
    std::string next_token = "t2";
    std::shared_ptr<FakeServer> server;

    void authorize(net::HttpRequest *req, const Credentials &c) const override {
        req->headers.emplace_back("Authorization", "Bearer " + c.access_token);
    }
    AuthResult refresh(const Credentials &current, const async::CancellationToken &) override {
        ++refreshes;
        std::this_thread::sleep_for(30ms);  // long enough for other requests to pile up
        if (mode == Mode::Rejected) return AuthResult::rejected("refresh token revoked");
        if (mode == Mode::Failed) return AuthResult::failed("no network", net::NetError::Connect);
        Credentials c = current;
        c.access_token = next_token;
        c.expires_at = 0;
        {
            std::lock_guard<std::mutex> lock(server->m);
            server->valid = next_token;
        }
        return AuthResult::ok(c);
    }
    void revoke(const Credentials &, const async::CancellationToken &) override { ++revokes; }
};

Credentials user(const std::string &token = "t1", const std::string &refresh = "r1") {
    Credentials c;
    c.account = "https://media.example|u1";
    c.user_id = "u1";
    c.user_name = "Alex";
    c.server = "https://media.example";
    c.access_token = token;
    c.refresh_token = refresh;
    return c;
}

struct Rig {
    std::string dir;
    std::unique_ptr<storage::KeyValueStore> kv;
    std::unique_ptr<storage::SecretStore> secrets;
    std::shared_ptr<CredentialStore> store;
    async::WorkerPool pool{3};
    async::MainDispatcher main;
    std::shared_ptr<FakeServer> server = std::make_shared<FakeServer>();
    std::shared_ptr<FakeProvider> provider = std::make_shared<FakeProvider>();
    std::unique_ptr<net::HttpClient> http;
    std::unique_ptr<Session> session;
    std::vector<std::string> transitions;
    int64_t clock = 1000000;
    async::Lifetime life;

    explicit Rig(SessionOptions opts = {}) {
        char tmpl[] = "/tmp/wnk-auth-XXXXXX";
        dir = mkdtemp(tmpl);
        kv = storage::open_file_store(dir);
        secrets = std::make_unique<storage::SecretStore>(*kv, nullptr, "org.wnk.test.secrets.v1");
        store = std::make_shared<CredentialStore>(*secrets, *kv);
        provider->server = server;
        http = std::make_unique<net::HttpClient>(server, pool, main);
        make_session(opts);
    }
    void make_session(SessionOptions opts = {}) {
        if (!opts.now) opts.now = [this] { return clock; };
        session = std::make_unique<Session>(store, provider, pool, main, opts);
        session->set_listener([this](SessionState from, SessionState to, const std::string &why) {
            transitions.push_back(std::string(session_state_name(from)) + ">" + session_state_name(to) +
                                  (why.empty() ? "" : ": " + why));
        });
    }
    ~Rig() {
        session.reset();
        pool.shutdown(2000ms);
        std::system(("rm -rf '" + dir + "'").c_str());
    }
    template <typename P>
    bool wait(P pred, std::chrono::milliseconds limit = 5000ms) {
        const auto end = async::Clock::now() + limit;
        while (async::Clock::now() < end) {
            main.drain();
            if (pred()) return true;
            std::this_thread::sleep_for(1ms);
        }
        return pred();
    }
    // Sends n requests through the session; returns the statuses as they arrive.
    std::shared_ptr<std::vector<long>> send(int n) {
        auto got = std::make_shared<std::vector<long>>();
        for (int i = 0; i < n; ++i) {
            net::HttpRequest req;
            req.url = "https://media.example/items/" + std::to_string(i);
            const bool sent = session->send(*http, req, {}, life.weak(), [got](net::HttpResponse r) {
                got->push_back(r.error == net::NetError::None ? r.status : -static_cast<long>(r.error));
            });
            CHECK(sent);
        }
        return got;
    }
};

}  // namespace

TEST("credentials: JSON round trip; describe never shows a token") {
    Credentials c = user();
    c.expires_at = 1003600;
    c.extra["device"] = "abc";
    Credentials back;
    REQUIRE(from_json(to_json(c), &back));
    CHECK_EQ(back.access_token, std::string("t1"));
    CHECK_EQ(back.refresh_token, std::string("r1"));
    CHECK_EQ(back.expires_at, int64_t{1003600});
    CHECK_EQ(back.extra["device"], std::string("abc"));
    const std::string d = describe(c, 1000000);
    CHECK(d.find("t1") == std::string::npos);
    CHECK(d.find("r1") == std::string::npos);
    CHECK(d.find("Alex") != std::string::npos);
    CHECK(d.find("expires in 3600 s") != std::string::npos);
    CHECK(!from_json("{}", &back));
    CHECK(!from_json(R"({"v":2,"account":"a","access":"x"})", &back));
    CHECK(!from_json(R"({"v":1,"account":"a"})", &back));  // no token
    CHECK(!from_json("not json", &back));
    CHECK(c.expires_within(1003550, 60));
    CHECK(!c.expires_within(1003500, 60));
    CHECK(!user().expires_within(INT64_MAX - 100, 60));  // no expiry
}

TEST("store: save, load, account list most recent first, remove; tokens only in the secret") {
    Rig r;
    Credentials a = user();
    Credentials b = user("t9", "");
    b.account = "https://other.example|u7";
    b.user_name = "Sam";
    REQUIRE(r.store->save(a) == storage::SecretStatus::Ok);
    REQUIRE(r.store->save(b) == storage::SecretStatus::Ok);
    auto list = r.store->accounts();
    REQUIRE(list.size() == 2u);
    CHECK_EQ(list[0].user_name, std::string("Sam"));
    CHECK_EQ(r.store->last_account(), b.account);
    REQUIRE(r.store->save(a) == storage::SecretStatus::Ok);  // used again: first
    CHECK_EQ(r.store->last_account(), a.account);
    CHECK_EQ(r.store->accounts().size(), 2u);
    storage::Entry index;
    REQUIRE(r.kv->get("auth.index", &index) == storage::Status::Ok);
    CHECK(index.value.find("t1") == std::string::npos);
    CHECK(index.value.find("r1") == std::string::npos);
    Credentials loaded;
    REQUIRE(r.store->load(a.account, &loaded) == storage::SecretStatus::Ok);
    CHECK_EQ(loaded.access_token, std::string("t1"));
    CHECK(r.store->remove(a.account) == storage::SecretStatus::Ok);
    CHECK(r.store->load(a.account, &loaded) == storage::SecretStatus::NotFound);
    CHECK_EQ(r.store->last_account(), b.account);
    CHECK(r.store->remove(a.account) == storage::SecretStatus::Ok);  // nothing left to remove
}

TEST("installation id: 32 hex digits, created once and kept") {
    Rig r;
    const std::string id = installation_id(*r.kv);
    CHECK_EQ(id.size(), 32u);
    CHECK(id.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK_EQ(installation_id(*r.kv), id);
}

TEST("session: restore finds nothing, then the account signed in with remember") {
    Rig r;
    SessionState got = SessionState::Restoring;
    r.session->restore([&](SessionState s) { got = s; });
    REQUIRE(r.wait([&] { return got != SessionState::Restoring; }));
    CHECK(got == SessionState::SignedOut);
    CHECK_EQ(r.transitions.back(), std::string("Restoring>SignedOut: no saved account"));
    storage::SecretStatus saved = storage::SecretStatus::Error;
    bool done = false;
    r.session->sign_in(user(), true, [&](storage::SecretStatus s) {
        saved = s;
        done = true;
    });
    CHECK(r.session->signed_in());
    REQUIRE(r.wait([&] { return done; }));
    CHECK(saved == storage::SecretStatus::Ok);
    // Next launch.
    r.make_session();
    got = SessionState::Restoring;
    r.session->restore([&](SessionState s) { got = s; });
    REQUIRE(r.wait([&] { return got != SessionState::Restoring; }));
    CHECK(got == SessionState::SignedIn);
    CHECK_EQ(r.session->credentials().access_token, std::string("t1"));
}

TEST("session: requests carry the token; signed out, send refuses") {
    Rig r;
    CHECK(!r.session->send(*r.http, net::HttpRequest{}, {}, r.life.weak(), [](net::HttpResponse) {}));
    r.session->sign_in(user(), false);
    auto got = r.send(2);
    REQUIRE(r.wait([&] { return got->size() == 2; }));
    CHECK_EQ((*got)[0], 200L);
    CHECK_EQ(r.server->count("t1"), 2u);
}

TEST("session: concurrent 401s share one refresh and are retried with the new token") {
    Rig r;
    r.session->sign_in(user(), true);
    {
        std::lock_guard<std::mutex> lock(r.server->m);
        r.server->valid = "server-rotated";  // t1 is no longer accepted
    }
    r.provider->next_token = "t2";
    r.server->delay = 5ms;
    auto got = r.send(4);
    REQUIRE(r.wait([&] { return got->size() == 4; }));
    for (long s : *got) CHECK_EQ(s, 200L);
    CHECK_EQ(r.provider->refreshes.load(), 1);
    CHECK_EQ(r.server->count("t2"), 4u);
    CHECK(r.session->state() == SessionState::SignedIn);
    CHECK_EQ(r.session->credentials().access_token, std::string("t2"));
    // The refreshed credentials were saved.
    REQUIRE(r.wait([&] {
        Credentials c;
        return r.store->load(user().account, &c) == storage::SecretStatus::Ok && c.access_token == "t2";
    }));
    // A request after the refresh goes straight out.
    auto more = r.send(1);
    REQUIRE(r.wait([&] { return more->size() == 1; }));
    CHECK_EQ(r.provider->refreshes.load(), 1);
}

TEST("session: a rejected refresh signs out, answers 401 and forgets the stored credentials") {
    Rig r;
    r.session->sign_in(user(), true);
    REQUIRE(r.wait([&] { return r.store->last_account() == user().account; }));
    {
        std::lock_guard<std::mutex> lock(r.server->m);
        r.server->valid = "none";
    }
    r.provider->mode = FakeProvider::Mode::Rejected;
    auto got = r.send(3);
    REQUIRE(r.wait([&] { return got->size() == 3; }));
    for (long s : *got) CHECK_EQ(s, 401L);
    CHECK_EQ(r.provider->refreshes.load(), 1);
    CHECK(r.session->state() == SessionState::SignedOut);
    CHECK(r.session->credentials().empty());
    CHECK_EQ(r.transitions.back(), std::string("Refreshing>SignedOut: the token could not be refreshed: refresh token revoked"));
    REQUIRE(r.wait([&] { return r.store->last_account().empty(); }));
}

TEST("session: a refresh that cannot reach the provider keeps the credentials") {
    Rig r;
    r.session->sign_in(user(), false);
    {
        std::lock_guard<std::mutex> lock(r.server->m);
        r.server->valid = "none";
    }
    r.provider->mode = FakeProvider::Mode::Failed;
    auto got = r.send(2);
    REQUIRE(r.wait([&] { return got->size() == 2; }));
    for (long s : *got) CHECK_EQ(s, 401L);  // their own 401s
    CHECK(r.session->state() == SessionState::SignedIn);
    CHECK_EQ(r.session->credentials().access_token, std::string("t1"));
}

TEST("session: a token about to expire is refreshed before the request goes out") {
    Rig r;
    Credentials c = user();
    c.expires_at = r.clock + 30;  // within the 60 s margin
    r.session->sign_in(c, false);
    {
        std::lock_guard<std::mutex> lock(r.server->m);
        r.server->valid = "t2";
    }
    auto got = r.send(2);
    REQUIRE(r.wait([&] { return got->size() == 2; }));
    CHECK_EQ((*got)[0], 200L);
    CHECK_EQ(r.server->count("t1"), 0u);  // the old token never went out
    CHECK_EQ(r.provider->refreshes.load(), 1);
}

TEST("session: a 401 without a refresh token signs out, unless sign_out_on_401 is off") {
    Rig r;
    r.session->sign_in(user("t1", ""), false);
    {
        std::lock_guard<std::mutex> lock(r.server->m);
        r.server->valid = "none";
    }
    auto got = r.send(1);
    REQUIRE(r.wait([&] { return got->size() == 1; }));
    CHECK_EQ((*got)[0], 401L);
    CHECK_EQ(r.provider->refreshes.load(), 0);
    CHECK(r.session->state() == SessionState::SignedOut);
    CHECK_EQ(r.transitions.back(), std::string("SignedIn>SignedOut: the server no longer accepts the credentials"));

    SessionOptions keep;
    keep.sign_out_on_401 = false;
    Rig k(keep);
    k.session->sign_in(user("t1", ""), false);
    {
        std::lock_guard<std::mutex> lock(k.server->m);
        k.server->valid = "none";
    }
    auto got2 = k.send(1);
    REQUIRE(k.wait([&] { return got2->size() == 1; }));
    CHECK_EQ((*got2)[0], 401L);
    CHECK(k.session->signed_in());
}

TEST("session: sign out revokes, removes the stored copy and fails waiting requests") {
    Rig r;
    r.session->sign_in(user(), true);
    REQUIRE(r.wait([&] { return r.store->last_account() == user().account; }));
    {
        std::lock_guard<std::mutex> lock(r.server->m);
        r.server->valid = "none";
    }
    auto got = r.send(1);
    REQUIRE(r.wait([&] { return r.session->state() == SessionState::Refreshing; }));
    bool done = false;
    r.session->sign_out([&] { done = true; });
    CHECK(r.session->credentials().empty());
    CHECK(!r.session->send(*r.http, net::HttpRequest{}, {}, r.life.weak(), [](net::HttpResponse) {}));
    REQUIRE(r.wait([&] { return done; }));
    REQUIRE(got->size() == 1u);
    CHECK_EQ((*got)[0], 401L);
    CHECK_EQ(r.provider->revokes.load(), 1);
    CHECK(r.session->state() == SessionState::SignedOut);
    CHECK_EQ(r.transitions.back(), std::string("SigningOut>SignedOut: signed out"));
    CHECK(r.store->last_account().empty());
    std::this_thread::sleep_for(60ms);  // the refresh that was running finishes and is ignored
    r.main.drain();
    CHECK(r.session->state() == SessionState::SignedOut);
}

TEST("session: a caller that went away gets no callback") {
    Rig r;
    r.session->sign_in(user(), false);
    r.server->delay = 30ms;
    int calls = 0;
    {
        async::Lifetime owner;
        net::HttpRequest req;
        req.url = "https://media.example/x";
        REQUIRE(r.session->send(*r.http, req, {}, owner.weak(), [&](net::HttpResponse) { ++calls; }));
    }
    std::this_thread::sleep_for(80ms);
    r.main.drain();
    CHECK_EQ(calls, 0);
}

namespace {
struct CaptureSink final : log::Sink {
    std::mutex *m;
    std::vector<std::string> *lines;
    CaptureSink(std::mutex *mu, std::vector<std::string> *l) : m(mu), lines(l) {}
    void write(const log::Record &, const std::string &line) override {
        std::lock_guard<std::mutex> lock(*m);
        lines->push_back(line);
    }
};
}  // namespace

TEST("session: no token reaches the log through sign-in, refresh, saving and sign-out") {
    std::mutex m;
    std::vector<std::string> lines;
    log::Logger logger(log::Level::Debug);
    logger.add_sink(std::make_unique<CaptureSink>(&m, &lines));
    log::set_default_logger(&logger);
    {
        Rig r;
        r.session->sign_in(user("SECRETACCESS42", "SECRETREFRESH42"), true);
        {
            std::lock_guard<std::mutex> lock(r.server->m);
            r.server->valid = "none";
        }
        r.provider->next_token = "SECRETNEW42";
        auto got = r.send(2);
        REQUIRE(r.wait([&] { return got->size() == 2; }));
        bool done = false;
        r.session->sign_out([&] { done = true; });
        REQUIRE(r.wait([&] { return done; }));
        r.make_session();
        bool restored = false;
        r.session->restore([&](SessionState) { restored = true; });
        REQUIRE(r.wait([&] { return restored; }));
        r.pool.shutdown(2000ms);
    }
    log::set_default_logger(nullptr);
    std::lock_guard<std::mutex> lock(m);
    CHECK(lines.size() > 5u);
    for (const std::string &l : lines) CHECK(l.find("SECRET") == std::string::npos);
    bool saw_refresh = false;
    for (const std::string &l : lines) saw_refresh |= l.find("refreshed: user 'Alex'") != std::string::npos;
    CHECK(saw_refresh);
}
