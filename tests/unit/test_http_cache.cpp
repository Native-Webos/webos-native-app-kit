// SPDX-License-Identifier: GPL-3.0-or-later
// The HTTP disk cache: freshness, revalidation, what is and is not stored, variants, invalidation,
// stale-on-error, persistence and eviction, through a scripted transport and a test clock.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/net/http_cache.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>

using namespace wnk;
using namespace wnk::net;

namespace {

class ScriptedTransport final : public HttpTransport {
 public:
    std::deque<HttpResponse> answers;
    std::vector<HttpRequest> requests;
    HttpResponse perform(const HttpRequest &req, const async::CancellationToken &) override {
        requests.push_back(req);
        if (answers.empty()) {
            HttpResponse r;
            r.error = NetError::Connect;
            r.detail = "nothing scripted";
            return r;
        }
        HttpResponse r = answers.front();
        answers.pop_front();
        return r;
    }
    std::string describe() const override { return "scripted"; }
    void add(long status, std::string body, Headers headers = {}) {
        HttpResponse r;
        r.status = status;
        r.body = std::move(body);
        r.headers = std::move(headers);
        answers.push_back(std::move(r));
    }
    void fail(NetError e) {
        HttpResponse r;
        r.error = e;
        answers.push_back(std::move(r));
    }
};

std::string temp_dir() {
    char tmpl[] = "/tmp/wnk-http-cache-XXXXXX";
    const char *d = ::mkdtemp(tmpl);
    return d != nullptr ? d : "";
}

void remove_dir(const std::string &dir) {
    if (DIR *d = ::opendir(dir.c_str())) {
        while (dirent *e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") ::unlink((dir + "/" + n).c_str());
        }
        ::closedir(d);
    }
    ::rmdir(dir.c_str());
}

std::string header(const HttpResponse &r, const std::string &name) { return r.header(name); }

struct Rig {
    std::string dir = temp_dir();
    int64_t clock = 1700000000;
    std::shared_ptr<ScriptedTransport> net = std::make_shared<ScriptedTransport>();
    std::shared_ptr<HttpCache> cache;
    std::unique_ptr<CachingTransport> t;
    explicit Rig(HttpCacheOptions o = {}) { open(std::move(o)); }
    ~Rig() { remove_dir(dir); }
    void open(HttpCacheOptions o = {}) {
        o.directory = dir;
        o.now_s = [this] { return clock; };
        cache = std::make_shared<HttpCache>(o);
        t = std::make_unique<CachingTransport>(net, cache);
    }
    HttpResponse get(const std::string &url, Headers h = {}) {
        HttpRequest r;
        r.url = url;
        r.headers = std::move(h);
        return t->perform(r, {});
    }
    std::string date() const { return format_http_date(clock); }
};

bool sent(const HttpRequest &r, const std::string &name, const std::string &value) {
    for (const auto &[k, v] : r.headers) {
        if (k == name && v == value) return true;
    }
    return false;
}

}  // namespace

TEST("http cache: IMF-fixdate dates both ways; other forms are invalid") {
    CHECK_EQ(parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT"), int64_t{784111777});
    CHECK_EQ(format_http_date(784111777), std::string("Sun, 06 Nov 1994 08:49:37 GMT"));
    CHECK_EQ(parse_http_date(format_http_date(1700000000)), int64_t{1700000000});
    CHECK_EQ(format_http_date(951782400), std::string("Tue, 29 Feb 2000 00:00:00 GMT"));
    CHECK_EQ(parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT"), int64_t{-1});
    CHECK_EQ(parse_http_date("Sun Nov  6 08:49:37 1994"), int64_t{-1});
    CHECK_EQ(parse_http_date("0"), int64_t{-1});
    CHECK_EQ(parse_http_date("Sun, 06 Foo 1994 08:49:37 GMT"), int64_t{-1});
}

TEST("http cache: a fresh response is answered from disk until max-age passes") {
    Rig r;
    REQUIRE(r.cache->usable());
    r.net->add(200, "v1", {{"cache-control", "max-age=60"}, {"date", r.date()}});
    HttpResponse a = r.get("http://h/a");
    CHECK_EQ(a.body, std::string("v1"));
    CHECK_EQ(header(a, "x-wnk-cache"), std::string("miss"));
    r.clock += 59;
    HttpResponse b = r.get("http://h/a");
    CHECK_EQ(b.status, 200L);
    CHECK_EQ(b.body, std::string("v1"));
    CHECK_EQ(header(b, "x-wnk-cache"), std::string("hit"));
    CHECK_EQ(r.net->requests.size(), size_t{1});
    r.clock += 2;  // stale, no validator: fetched again
    r.net->add(200, "v2", {{"cache-control", "max-age=60"}});
    CHECK_EQ(r.get("http://h/a").body, std::string("v2"));
    CHECK_EQ(r.net->requests.size(), size_t{2});
    // An Age the server reports counts.
    r.net->add(200, "aged", {{"cache-control", "max-age=60"}, {"age", "50"}});
    r.get("http://h/aged");
    r.clock += 11;
    r.net->add(200, "aged2", {{"cache-control", "max-age=60"}});
    CHECK_EQ(r.get("http://h/aged").body, std::string("aged2"));
}

TEST("http cache: Expires against Date; an invalid Expires is already stale") {
    Rig r;
    r.net->add(200, "e", {{"expires", format_http_date(r.clock + 100)}, {"date", r.date()}});
    r.get("http://h/e");
    r.clock += 99;
    CHECK_EQ(header(r.get("http://h/e"), "x-wnk-cache"), std::string("hit"));
    r.net->add(200, "bad", {{"expires", "0"}, {"etag", "\"x\""}});
    r.get("http://h/bad");
    r.net->add(304, "", {});
    CHECK_EQ(header(r.get("http://h/bad"), "x-wnk-cache"), std::string("revalidated"));
}

TEST("http cache: a stale copy with an ETag is revalidated; 304 refreshes its headers") {
    Rig r;
    r.net->add(200, "body", {{"etag", "\"abc\""}, {"cache-control", "max-age=10"}, {"x-extra", "1"}});
    r.get("http://h/r");
    r.clock += 20;
    r.net->add(304, "", {{"etag", "\"abc\""}, {"cache-control", "max-age=300"}, {"content-length", "0"}});
    HttpResponse b = r.get("http://h/r");
    REQUIRE(r.net->requests.size() == 2);
    CHECK(sent(r.net->requests[1], "If-None-Match", "\"abc\""));
    CHECK_EQ(b.status, 200L);
    CHECK_EQ(b.body, std::string("body"));
    CHECK_EQ(header(b, "x-wnk-cache"), std::string("revalidated"));
    CHECK_EQ(header(b, "x-extra"), std::string("1"));
    CHECK_EQ(header(b, "cache-control"), std::string("max-age=300"));
    r.clock += 200;  // fresh for 300 s from the revalidation
    CHECK_EQ(header(r.get("http://h/r"), "x-wnk-cache"), std::string("hit"));
    CHECK_EQ(r.net->requests.size(), size_t{2});
    // A changed resource: 200 replaces it.
    r.clock += 400;
    r.net->add(200, "new", {{"etag", "\"def\""}});
    CHECK_EQ(r.get("http://h/r").body, std::string("new"));
    r.net->add(304, "", {});
    CHECK_EQ(r.get("http://h/r").body, std::string("new"));
    CHECK(sent(r.net->requests.back(), "If-None-Match", "\"def\""));
}

TEST("http cache: Last-Modified gives If-Modified-Since; no-cache always revalidates") {
    Rig r;
    const std::string lm = "Wed, 21 Oct 2015 07:28:00 GMT";
    r.net->add(200, "lm", {{"last-modified", lm}, {"cache-control", "no-cache, max-age=3600"}});
    r.get("http://h/lm");
    r.net->add(304, "", {});
    CHECK_EQ(r.get("http://h/lm").body, std::string("lm"));
    REQUIRE(r.net->requests.size() == 2);
    CHECK(sent(r.net->requests[1], "If-Modified-Since", lm));
}

TEST("http cache: what is not stored") {
    Rig r;
    const Headers fresh = {{"cache-control", "max-age=600"}};
    r.net->add(200, "a", {{"cache-control", "no-store, max-age=600"}});
    r.net->add(200, "b", {});  // neither freshness nor a validator
    r.net->add(200, "c", {{"cache-control", "max-age=600"}, {"vary", "*"}});
    r.net->add(500, "d", fresh);
    r.net->add(206, "e", fresh);
    r.net->add(200, std::string((4u << 20) + 1, 'x'), fresh);
    for (const char *u : {"http://h/1", "http://h/2", "http://h/3", "http://h/4", "http://h/5", "http://h/6"}) r.get(u);
    CHECK_EQ(r.cache->entries(), size_t{0});
    r.net->add(404, "gone", fresh);  // a cacheable error
    r.get("http://h/404");
    CHECK_EQ(r.get("http://h/404").status, 404L);
    CHECK_EQ(r.net->requests.size(), size_t{7});
    // Set-Cookie and framing headers never reach the disk.
    r.net->add(200, "k", {{"cache-control", "max-age=600"}, {"set-cookie", "s=1"}, {"content-encoding", "gzip"}});
    r.get("http://h/cookie");
    HttpResponse hit = r.get("http://h/cookie");
    CHECK_EQ(header(hit, "x-wnk-cache"), std::string("hit"));
    CHECK(header(hit, "set-cookie").empty());
    CHECK(header(hit, "content-encoding").empty());
}

TEST("http cache: Range, Authorization, own conditionals and no-store requests bypass it") {
    Rig r;
    r.net->add(200, "x", {{"cache-control", "max-age=600"}});
    r.get("http://h/x");
    for (const Headers &h : std::vector<Headers>{{{"Range", "bytes=0-1"}}, {{"Authorization", "Bearer t"}},
                                                  {{"If-None-Match", "\"z\""}}, {{"Cache-Control", "no-store"}}}) {
        r.net->add(200, "net", {{"cache-control", "max-age=600"}});
        HttpResponse b = r.get("http://h/x", h);
        CHECK_EQ(b.body, std::string("net"));
        CHECK_EQ(header(b, "x-wnk-cache"), std::string("bypass"));
    }
    CHECK_EQ(r.get("http://h/x").body, std::string("x"));  // the stored copy is untouched
    // A refresh: Cache-Control: no-cache on the request revalidates even a fresh copy.
    r.net->add(200, "refreshed", {{"cache-control", "max-age=600"}});
    CHECK_EQ(r.get("http://h/x", {{"Cache-Control", "no-cache"}}).body, std::string("refreshed"));
    CHECK_EQ(r.get("http://h/x").body, std::string("refreshed"));
}

TEST("http cache: Vary keeps the request headers a response was for") {
    Rig r;
    r.net->add(200, "en", {{"cache-control", "max-age=600"}, {"vary", "Accept-Language"}});
    r.get("http://h/v", {{"Accept-Language", "en"}});
    CHECK_EQ(header(r.get("http://h/v", {{"Accept-Language", "en"}}), "x-wnk-cache"), std::string("hit"));
    r.net->add(200, "fr", {{"cache-control", "max-age=600"}, {"vary", "Accept-Language"}});
    CHECK_EQ(r.get("http://h/v", {{"accept-language", "fr"}}).body, std::string("fr"));
    CHECK_EQ(r.get("http://h/v", {{"Accept-Language", "fr"}}).body, std::string("fr"));  // one variant kept: the last
    CHECK_EQ(r.net->requests.size(), size_t{2});
}

TEST("http cache: a successful POST, PUT or DELETE drops the stored copy of its URL") {
    Rig r;
    r.net->add(200, "list", {{"cache-control", "max-age=600"}});
    r.get("http://h/items");
    r.net->add(201, "", {});
    HttpRequest post;
    post.method = "POST";
    post.url = "http://h/items";
    CHECK_EQ(header(r.t->perform(post, {}), "x-wnk-cache"), std::string("bypass"));
    r.net->add(200, "list2", {{"cache-control", "max-age=600"}});
    CHECK_EQ(r.get("http://h/items").body, std::string("list2"));
    r.net->add(500, "", {});  // a failed change keeps it
    post.method = "DELETE";
    r.t->perform(post, {});
    CHECK_EQ(header(r.get("http://h/items"), "x-wnk-cache"), std::string("hit"));
}

TEST("http cache: the stored copy on errors only where allowed") {
    {
        Rig r;  // default: an error is an error
        r.net->add(200, "old", {{"etag", "\"1\""}, {"cache-control", "max-age=10"}});
        r.get("http://h/s");
        r.clock += 20;
        r.net->fail(NetError::Connect);
        HttpResponse b = r.get("http://h/s");
        CHECK(b.error == NetError::Connect);
        CHECK_EQ(header(b, "x-wnk-cache"), std::string("miss"));
        // stale-if-error from the server allows it, within its window.
        r.net->add(200, "sie", {{"cache-control", "max-age=10, stale-if-error=60"}});
        r.clock += 100;  // the stored "old" is stale; this replaces it
        r.get("http://h/s");
        r.clock += 50;
        r.net->add(503, "busy", {});
        HttpResponse c = r.get("http://h/s");
        CHECK_EQ(c.status, 200L);
        CHECK_EQ(c.body, std::string("sie"));
        CHECK_EQ(header(c, "x-wnk-cache"), std::string("stale"));
        r.clock += 30;  // 70 s past fresh: beyond the window
        r.net->fail(NetError::Timeout);
        CHECK(r.get("http://h/s").error == NetError::Timeout);
    }
    {
        HttpCacheOptions o;
        o.stale_if_error = true;  // the app would rather show old data offline
        o.max_stale_s = 3600;
        Rig r(o);
        r.net->add(200, "offline", {{"etag", "\"1\""}, {"cache-control", "no-cache"}});
        r.get("http://h/o");
        r.clock += 600;
        r.net->fail(NetError::Dns);
        HttpResponse b = r.get("http://h/o");
        CHECK_EQ(b.body, std::string("offline"));
        CHECK_EQ(header(b, "x-wnk-cache"), std::string("stale"));
        r.net->add(200, "mr", {{"cache-control", "max-age=10, must-revalidate"}});
        r.get("http://h/mr");
        r.clock += 20;
        r.net->fail(NetError::Dns);
        CHECK(r.get("http://h/mr").error == NetError::Dns);  // must-revalidate: never stale
        r.clock += 7200;
        r.net->fail(NetError::Dns);
        CHECK(r.get("http://h/o").error == NetError::Dns);  // past max_stale_s
    }
}

TEST("http cache: entries survive a restart; LRU eviction; corrupt files are misses") {
    HttpCacheOptions o;
    o.max_bytes = 2000;
    Rig r(o);
    const Headers fresh = {{"cache-control", "max-age=600"}};
    r.net->add(200, std::string(600, 'a'), fresh);
    r.net->add(200, std::string(600, 'b'), fresh);
    r.get("http://h/a");
    r.get("http://h/b");
    r.get("http://h/a");  // a is now the most recent
    CHECK_EQ(r.cache->entries(), size_t{2});
    r.net->add(200, std::string(600, 'c'), fresh);
    r.get("http://h/c");  // over 2000 bytes with headers: b, the least recent, goes
    CHECK_EQ(r.cache->entries(), size_t{2});
    CHECK(r.cache->bytes() <= 2000);
    r.open(o);  // a new process
    CHECK_EQ(r.cache->entries(), size_t{2});
    CHECK_EQ(header(r.get("http://h/a"), "x-wnk-cache"), std::string("hit"));
    CHECK_EQ(header(r.get("http://h/c"), "x-wnk-cache"), std::string("hit"));
    r.net->add(200, "b again", fresh);
    CHECK_EQ(header(r.get("http://h/b"), "x-wnk-cache"), std::string("miss"));
    // Damage every entry file and leave a stray temporary.
    if (DIR *d = ::opendir(r.dir.c_str())) {
        while (dirent *e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 3 && n.compare(n.size() - 3, 3, ".hc") == 0) {
                std::ofstream(r.dir + "/" + n, std::ios::trunc) << "WNKHC1\nhttp://h/a\n";
            }
        }
        ::closedir(d);
    }
    std::ofstream(r.dir + "/x.hc.1-1.tmp") << "partial";
    r.open(o);
    struct stat st{};
    CHECK(::stat((r.dir + "/x.hc.1-1.tmp").c_str(), &st) != 0);
    r.net->add(200, "fresh a", fresh);
    CHECK_EQ(r.get("http://h/a").body, std::string("fresh a"));
}

TEST("http cache: without a directory it only passes requests through") {
    HttpCacheOptions o;
    auto cache = std::make_shared<HttpCache>(o);
    CHECK(!cache->usable());
    auto net = std::make_shared<ScriptedTransport>();
    CachingTransport t(net, cache);
    net->add(200, "x", {{"cache-control", "max-age=600"}});
    net->add(200, "y", {{"cache-control", "max-age=600"}});
    HttpRequest r;
    r.url = "http://h/x";
    CHECK_EQ(t.perform(r, {}).body, std::string("x"));
    CHECK_EQ(t.perform(r, {}).body, std::string("y"));
    CHECK_EQ(t.stats().bypassed, uint64_t{2});
}
