// SPDX-License-Identifier: GPL-3.0-or-later
//
// A private HTTP cache on disk (RFC 9111, the parts a client app needs) and a transport that uses it.
//
// CachingTransport wraps any HttpTransport. A GET it may answer from the cache:
//   - fresh (Cache-Control max-age, else Expires against Date): answered from disk, no request;
//   - stale or no-cache, with an ETag or Last-Modified: a conditional request; 304 refreshes the
//     stored headers and answers with the stored body (HTTP 200), anything else replaces it;
//   - the network fails or the server answers 5xx: the stored copy, if the response allowed it
//     (stale-if-error=N) or HttpCacheOptions::stale_if_error says so, and never past must-revalidate.
// A response is stored when it is 200, 203, 301, 404 or 410, carries freshness or a validator, is not
// no-store and not Vary: *, and fits max_entry_bytes. Requests with Range, with Authorization (unless
// the response says public) or with Cache-Control: no-store bypass the cache; Cache-Control: no-cache
// on the request forces a revalidation (a "refresh" button). A successful POST, PUT, PATCH or DELETE
// drops the stored copy of its URL. Nothing is cached without either freshness or a validator: there
// is no heuristic freshness, so an app never sees older data than HTTP allows.
//
// Every answer carries an "x-wnk-cache" header: "hit" (from disk), "revalidated" (304), "stale"
// (served after an error), "miss" (stored or not), or "bypass".
//
// stream() (media, byte ranges) always goes to the network. Thread-safe; entries are files written
// whole and renamed into place, so a crash leaves at worst a stray .tmp, removed on the next start.
#pragma once

#include "wnk/net/http.hpp"

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace wnk::net {

struct HttpCacheOptions {
    std::string directory;              // created if missing; empty = the cache is not usable
    uint64_t max_bytes = 32u << 20;     // all entries; least recently used ones go first
    size_t max_entry_bytes = 4u << 20;  // larger bodies are not stored
    // Serve the stored copy when the network fails or the server answers 5xx, even if the response
    // did not say stale-if-error (an app that would rather show old data offline than an error).
    bool stale_if_error = false;
    int64_t max_stale_s = 7 * 24 * 3600;  // how old a copy served on error may be (past its freshness)
    std::function<int64_t()> now_s;       // seconds since the epoch; tests set it (default: the clock)
};

// "Sun, 06 Nov 1994 08:49:37 GMT" (IMF-fixdate) in seconds since the epoch, or -1.
int64_t parse_http_date(const std::string &text);
std::string format_http_date(int64_t seconds);

class HttpCache {
 public:
    explicit HttpCache(HttpCacheOptions options);
    bool usable() const { return usable_; }
    const HttpCacheOptions &options() const { return opt_; }
    uint64_t bytes() const;
    size_t entries() const;
    void clear();

    struct Entry {
        std::string url;
        long status = 0;
        Headers headers;  // of the stored response, names lower-cased
        std::string body;
        std::string effective_url;
        Headers vary;        // request header name (lower case) → value the response was for
        int64_t request_s = 0;   // when the request that produced it was sent
        int64_t response_s = 0;  // when its response arrived (or was last revalidated)
    };
    std::optional<Entry> get(const std::string &url);
    bool put(const Entry &e);
    void remove(const std::string &url);
    int64_t now() const;

 private:
    std::string file_for(const std::string &url) const;
    void evict_locked(uint64_t target);
    HttpCacheOptions opt_;
    bool usable_ = false;
    mutable std::mutex mutex_;
    uint64_t bytes_ = 0;
    std::list<std::pair<std::string, uint64_t>> lru_;  // (file name, size); front = most recent
    std::map<std::string, std::list<std::pair<std::string, uint64_t>>::iterator> index_;
};

struct HttpCacheStats {
    uint64_t hits = 0, revalidated = 0, stale = 0, misses = 0, stored = 0, bypassed = 0;
};

class CachingTransport final : public HttpTransport {
 public:
    CachingTransport(std::shared_ptr<HttpTransport> inner, std::shared_ptr<HttpCache> cache);
    HttpResponse perform(const HttpRequest &req, const async::CancellationToken &token) override;
    HttpResponse stream(const HttpRequest &req, const async::CancellationToken &token, const HttpStreamSink &sink) override;
    std::string describe() const override;
    HttpCacheStats stats() const;
    HttpCache &cache() { return *cache_; }

 private:
    std::shared_ptr<HttpTransport> inner_;
    std::shared_ptr<HttpCache> cache_;
    mutable std::mutex stats_mutex_;
    HttpCacheStats stats_;
};

}  // namespace wnk::net
