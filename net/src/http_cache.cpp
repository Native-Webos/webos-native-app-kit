// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/net/http_cache.hpp"

#include "wnk/log.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace wnk::net {

namespace {

constexpr const char *kCat = "http.cache";
constexpr const char *kMagic = "WNKHC1\n";

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string header_of(const Headers &h, const std::string &name) {
    for (const auto &[k, v] : h) {
        if (lower(k) == name) return v;
    }
    return {};
}

bool has_header(const Headers &h, const std::string &name) {
    return std::any_of(h.begin(), h.end(), [&](const auto &kv) { return lower(kv.first) == name; });
}

// Cache-Control directives, names lower-cased, values unquoted ("" for a bare directive).
std::map<std::string, std::string> cache_control(const std::string &value) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < value.size()) {
        size_t comma = value.find(',', i);
        if (comma == std::string::npos) comma = value.size();
        const std::string part = trim(value.substr(i, comma - i));
        const size_t eq = part.find('=');
        if (!part.empty()) {
            std::string v = eq == std::string::npos ? "" : trim(part.substr(eq + 1));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            out[lower(trim(part.substr(0, eq)))] = v;
        }
        i = comma + 1;
    }
    return out;
}

int64_t seconds_of(const std::map<std::string, std::string> &cc, const std::string &name, int64_t fallback = -1) {
    auto it = cc.find(name);
    if (it == cc.end()) return fallback;
    char *end = nullptr;
    const long long v = std::strtoll(it->second.c_str(), &end, 10);
    return end != nullptr && *end == '\0' && !it->second.empty() && v >= 0 ? v : 0;  // malformed: treat as 0 (stale)
}

uint64_t fnv1a(const std::string &s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

bool make_dirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string cur = path.substr(0, i);
            if (::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
    }
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool read_file(const std::string &path, std::string *out, size_t limit) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    out->clear();
    char buf[65536];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) {
        out->append(buf, static_cast<size_t>(n));
        if (out->size() > limit) {
            ::close(fd);
            return false;
        }
    }
    ::close(fd);
    return n == 0;
}

bool write_file(const std::string &path, const std::string &data) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return ::close(fd) == 0;
}

bool clean(const std::string &s) { return s.find('\n') == std::string::npos && s.find('\r') == std::string::npos; }

std::string encode(const HttpCache::Entry &e) {
    std::string s = kMagic;
    s += e.url + "\n" + e.effective_url + "\n" + std::to_string(e.status) + "\n" + std::to_string(e.request_s) + "\n" +
         std::to_string(e.response_s) + "\n" + std::to_string(e.vary.size()) + "\n";
    for (const auto &[k, v] : e.vary) s += k + "\t" + v + "\n";
    s += std::to_string(e.headers.size()) + "\n";
    for (const auto &[k, v] : e.headers) s += k + "\t" + v + "\n";
    s += std::to_string(e.body.size()) + "\n";
    return s + e.body;
}

bool decode(const std::string &s, HttpCache::Entry *e) {
    if (s.compare(0, std::strlen(kMagic), kMagic) != 0) return false;
    size_t pos = std::strlen(kMagic);
    auto line = [&](std::string *out) {
        const size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) return false;
        *out = s.substr(pos, nl - pos);
        pos = nl + 1;
        return true;
    };
    auto number = [&](int64_t *out) {
        std::string l;
        if (!line(&l) || l.empty()) return false;
        char *end = nullptr;
        *out = std::strtoll(l.c_str(), &end, 10);
        return end != nullptr && *end == '\0';
    };
    auto pairs = [&](Headers *out) {
        int64_t n = 0;
        if (!number(&n) || n < 0 || n > 1000) return false;
        for (int64_t i = 0; i < n; ++i) {
            std::string l;
            if (!line(&l)) return false;
            const size_t tab = l.find('\t');
            if (tab == std::string::npos) return false;
            out->emplace_back(l.substr(0, tab), l.substr(tab + 1));
        }
        return true;
    };
    int64_t status = 0, size = 0;
    if (!line(&e->url) || !line(&e->effective_url) || !number(&status) || !number(&e->request_s) ||
        !number(&e->response_s) || !pairs(&e->vary) || !pairs(&e->headers) || !number(&size)) {
        return false;
    }
    if (size < 0 || static_cast<size_t>(size) != s.size() - pos) return false;  // truncated
    e->status = static_cast<long>(status);
    e->body = s.substr(pos);
    return true;
}

// RFC 9111 §4.2.1 and §4.2.3.
int64_t freshness_lifetime(const HttpCache::Entry &e) {
    const auto cc = cache_control(header_of(e.headers, "cache-control"));
    if (cc.count("no-cache")) return 0;
    const int64_t max_age = seconds_of(cc, "max-age");
    if (max_age >= 0) return max_age;
    const std::string expires = header_of(e.headers, "expires");
    if (expires.empty()) return 0;
    const int64_t exp = parse_http_date(expires);
    if (exp < 0) return 0;  // invalid Expires: already expired
    const int64_t date = parse_http_date(header_of(e.headers, "date"));
    return std::max<int64_t>(0, exp - (date >= 0 ? date : e.response_s));
}

int64_t current_age(const HttpCache::Entry &e, int64_t now) {
    const int64_t date = parse_http_date(header_of(e.headers, "date"));
    const int64_t apparent = date >= 0 ? std::max<int64_t>(0, e.response_s - date) : 0;
    char *end = nullptr;
    const std::string age_text = header_of(e.headers, "age");
    int64_t age = std::strtoll(age_text.c_str(), &end, 10);
    if (age_text.empty() || end == nullptr || *end != '\0' || age < 0) age = 0;
    const int64_t corrected = age + std::max<int64_t>(0, e.response_s - e.request_s);
    return std::max(apparent, corrected) + std::max<int64_t>(0, now - e.response_s);
}

bool storable_status(long s) { return s == 200 || s == 203 || s == 301 || s == 404 || s == 410; }

HttpResponse from_entry(const HttpCache::Entry &e, const char *how) {
    HttpResponse r;
    r.status = e.status;
    r.headers = e.headers;
    r.headers.emplace_back("x-wnk-cache", how);
    r.body = e.body;
    r.effective_url = e.effective_url.empty() ? e.url : e.effective_url;
    r.detail = std::string("http cache: ") + how;
    return r;
}

void mark(HttpResponse *r, const char *how) { r->headers.emplace_back("x-wnk-cache", how); }

}  // namespace

int64_t parse_http_date(const std::string &text) {
    // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT". RFC 850 and asctime forms are obsolete; a
    // recipient may treat what it cannot read as invalid.
    static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::string t = trim(text);
    char wday[4] = {}, mon[4] = {}, zone[4] = {};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (std::sscanf(t.c_str(), "%3[A-Za-z], %2d %3s %4d %2d:%2d:%2d %3s", wday, &day, mon, &year, &hh, &mm, &ss, zone) != 8 ||
        std::strcmp(zone, "GMT") != 0 || t.size() < 4 || t[3] != ',') {
        return -1;
    }
    int month = -1;
    for (int i = 0; i < 12; ++i) {
        if (std::strcmp(mon, kMonths[i]) == 0) month = i;
    }
    if (month < 0 || day < 1 || day > 31 || year < 1970 || hh > 23 || mm > 59 || ss > 60) return -1;
    // Days from the civil date (Howard Hinnant's algorithm), no time zone involved.
    const int y = year - (month < 2 ? 1 : 0);
    const int era = y / 400;
    const int yoe = y - era * 400;
    const int mp = (month + 10) % 12;  // month is 0-based here (the algorithm counts from March)
    const int doy = (153 * mp + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

std::string format_http_date(int64_t seconds) {
    static const char *kDays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};  // 1970-01-01 was a Thursday
    static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int64_t days = seconds >= 0 ? seconds / 86400 : 0;
    const int64_t rem = seconds >= 0 ? seconds % 86400 : 0;
    const int64_t z = days + 719468;
    const int64_t era = z / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    char b[96];
    std::snprintf(b, sizeof b, "%s, %02lld %s %04lld %02lld:%02lld:%02lld GMT", kDays[days % 7], static_cast<long long>(d),
                  kMonths[m - 1], static_cast<long long>(y), static_cast<long long>(rem / 3600),
                  static_cast<long long>(rem / 60 % 60), static_cast<long long>(rem % 60));
    return b;
}

// ---- HttpCache -----------------------------------------------------------------------------------
//
// One file per URL: "<fnv1a64-hex>.hc", the encoded entry. The URL is stored and compared on read,
// so a hash collision is a miss, not someone else's response.

HttpCache::HttpCache(HttpCacheOptions options) : opt_(std::move(options)) {
    if (opt_.directory.empty() || !make_dirs(opt_.directory)) return;
    usable_ = true;
    std::vector<std::pair<time_t, std::pair<std::string, uint64_t>>> found;
    if (DIR *d = ::opendir(opt_.directory.c_str())) {
        while (dirent *e = ::readdir(d)) {
            const std::string name = e->d_name;
            const std::string full = opt_.directory + "/" + name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
                ::unlink(full.c_str());  // interrupted write
                continue;
            }
            if (name.size() <= 3 || name.compare(name.size() - 3, 3, ".hc") != 0) continue;
            struct stat st{};
            if (::lstat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            found.push_back({st.st_mtime, {name, static_cast<uint64_t>(st.st_size)}});
        }
        ::closedir(d);
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &f : found) {  // oldest first, so the newest ends up at the front
        lru_.push_front(f.second);
        index_[f.second.first] = lru_.begin();
        bytes_ += f.second.second;
    }
    evict_locked(opt_.max_bytes);
}

int64_t HttpCache::now() const {
    if (opt_.now_s) return opt_.now_s();
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string HttpCache::file_for(const std::string &url) const {
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.hc", static_cast<unsigned long long>(fnv1a(url)));
    return name;
}

std::optional<HttpCache::Entry> HttpCache::get(const std::string &url) {
    if (!usable_) return std::nullopt;
    const std::string name = file_for(url);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(name);
        if (it == index_.end()) return std::nullopt;
        lru_.splice(lru_.begin(), lru_, it->second);
    }
    std::string data;
    Entry e;
    if (!read_file(opt_.directory + "/" + name, &data, opt_.max_entry_bytes + 65536) || !decode(data, &e) || e.url != url) {
        // Unreadable, truncated or another URL with the same hash: drop it.
        remove(url);
        return std::nullopt;
    }
    return e;
}

bool HttpCache::put(const Entry &e) {
    if (!usable_ || e.body.size() > opt_.max_entry_bytes || !clean(e.url) || !clean(e.effective_url)) return false;
    for (const auto *list : {&e.headers, &e.vary}) {
        for (const auto &[k, v] : *list) {
            if (!clean(k) || !clean(v) || k.find('\t') != std::string::npos) return false;
        }
    }
    const std::string name = file_for(e.url);
    const std::string data = encode(e);
    const std::string path = opt_.directory + "/" + name;
    static std::atomic<uint64_t> serial{0};  // two writers of one URL never share a temporary file
    const std::string tmp = path + "." + std::to_string(::getpid()) + "-" + std::to_string(++serial) + ".tmp";
    if (!write_file(tmp, data) || ::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        WNK_LOGW(kCat, "cannot write an entry to %s: %s", opt_.directory.c_str(), std::strerror(errno));
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(name);
    if (it != index_.end()) {
        bytes_ -= it->second->second;
        lru_.erase(it->second);
        index_.erase(it);
    }
    lru_.push_front({name, data.size()});
    index_[name] = lru_.begin();
    bytes_ += data.size();
    evict_locked(opt_.max_bytes);
    return true;
}

void HttpCache::remove(const std::string &url) {
    if (!usable_) return;
    const std::string name = file_for(url);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(name);
    if (it == index_.end()) return;
    ::unlink((opt_.directory + "/" + name).c_str());
    bytes_ -= it->second->second;
    lru_.erase(it->second);
    index_.erase(it);
}

void HttpCache::evict_locked(uint64_t target) {
    while (bytes_ > target && !lru_.empty()) {
        const auto &[name, size] = lru_.back();
        ::unlink((opt_.directory + "/" + name).c_str());
        bytes_ -= size;
        index_.erase(name);
        lru_.pop_back();
    }
}

uint64_t HttpCache::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

size_t HttpCache::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.size();
}

void HttpCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    evict_locked(0);
}

// ---- CachingTransport ----------------------------------------------------------------------------

CachingTransport::CachingTransport(std::shared_ptr<HttpTransport> inner, std::shared_ptr<HttpCache> cache)
    : inner_(std::move(inner)), cache_(std::move(cache)) {}

std::string CachingTransport::describe() const { return inner_->describe() + " + disk cache"; }

HttpCacheStats CachingTransport::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

HttpResponse CachingTransport::stream(const HttpRequest &req, const async::CancellationToken &token, const HttpStreamSink &sink) {
    return inner_->stream(req, token, sink);
}

HttpResponse CachingTransport::perform(const HttpRequest &req, const async::CancellationToken &token) {
    auto count = [this](uint64_t HttpCacheStats::*field) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++(stats_.*field);
    };
    const std::string method = req.method.empty() ? "GET" : req.method;
    if (method != "GET") {
        HttpResponse r = inner_->perform(req, token);
        // A change through the URL makes what is stored for it out of date (RFC 9111 §4.4).
        if (r.ok() && method != "HEAD" && method != "OPTIONS" && cache_->usable()) cache_->remove(req.url);
        mark(&r, "bypass");
        count(&HttpCacheStats::bypassed);
        return r;
    }
    const auto req_cc = cache_control(header_of(req.headers, "cache-control"));
    if (!cache_->usable() || has_header(req.headers, "range") || has_header(req.headers, "authorization") ||
        has_header(req.headers, "if-none-match") || has_header(req.headers, "if-modified-since") || req_cc.count("no-store")) {
        HttpResponse r = inner_->perform(req, token);
        mark(&r, "bypass");
        count(&HttpCacheStats::bypassed);
        return r;
    }
    const bool force = req_cc.count("no-cache") || seconds_of(req_cc, "max-age") == 0 ||
                       lower(header_of(req.headers, "pragma")).find("no-cache") != std::string::npos;

    std::optional<HttpCache::Entry> stored = cache_->get(req.url);
    if (stored) {
        for (const auto &[name, value] : stored->vary) {
            if (header_of(req.headers, name) != value) {
                stored.reset();  // stored for another variant
                break;
            }
        }
    }
    const int64_t now = cache_->now();
    if (stored && !force && freshness_lifetime(*stored) > current_age(*stored, now)) {
        count(&HttpCacheStats::hits);
        return from_entry(*stored, "hit");
    }

    HttpRequest net = req;
    if (stored) {
        const std::string etag = header_of(stored->headers, "etag");
        const std::string modified = header_of(stored->headers, "last-modified");
        if (!etag.empty()) net.headers.emplace_back("If-None-Match", etag);
        if (!modified.empty()) net.headers.emplace_back("If-Modified-Since", modified);
    }
    const int64_t request_s = cache_->now();
    HttpResponse r = inner_->perform(net, token);
    const int64_t response_s = cache_->now();
    if (r.error == NetError::Cancelled) return r;

    if (stored && r.error == NetError::None && r.status == 304) {
        // Fresh again: the 304's headers update the stored ones (RFC 9111 §4.3.4).
        for (const auto &[k, v] : r.headers) {
            const std::string name = lower(k);
            if (name == "content-length" || name == "content-encoding" || name == "transfer-encoding") continue;
            auto &h = stored->headers;
            h.erase(std::remove_if(h.begin(), h.end(), [&](const auto &kv) { return lower(kv.first) == name; }), h.end());
            h.emplace_back(name, v);
        }
        stored->request_s = request_s;
        stored->response_s = response_s;
        cache_->put(*stored);
        count(&HttpCacheStats::revalidated);
        return from_entry(*stored, "revalidated");
    }

    if (stored && (r.error != NetError::None || r.status >= 500)) {
        const auto cc = cache_control(header_of(stored->headers, "cache-control"));
        const int64_t past = current_age(*stored, now) - freshness_lifetime(*stored);
        const int64_t allowed = std::max(seconds_of(cc, "stale-if-error", 0), cache_->options().stale_if_error ? cache_->options().max_stale_s : 0);
        if (!cc.count("must-revalidate") && !cc.count("no-store") && past <= allowed) {
            WNK_LOGI(kCat, "%s: %s; serving the stored copy (%lld s past fresh)", req.url.c_str(),
                     r.error != NetError::None ? error_name(r.error) : ("HTTP " + std::to_string(r.status)).c_str(),
                     static_cast<long long>(past));
            count(&HttpCacheStats::stale);
            return from_entry(*stored, "stale");
        }
    }

    count(&HttpCacheStats::misses);
    if (r.error != NetError::None) {
        mark(&r, "miss");
        return r;
    }
    const auto cc = cache_control(header_of(r.headers, "cache-control"));
    const std::string vary = header_of(r.headers, "vary");
    const bool validator = !header_of(r.headers, "etag").empty() || !header_of(r.headers, "last-modified").empty();
    const bool fresh_info = seconds_of(cc, "max-age") >= 0 || !header_of(r.headers, "expires").empty();
    bool store = storable_status(r.status) && !cc.count("no-store") && trim(vary) != "*" && (validator || fresh_info) &&
                 r.body.size() <= cache_->options().max_entry_bytes;
    if (store) {
        HttpCache::Entry e;
        e.url = req.url;
        e.status = r.status;
        e.effective_url = r.effective_url;
        e.body = r.body;
        e.request_s = request_s;
        e.response_s = response_s;
        for (const auto &[k, v] : r.headers) {
            const std::string name = lower(k);
            // The body is stored as delivered (already decoded); framing headers would lie about it.
            if (name == "content-length" || name == "content-encoding" || name == "transfer-encoding" || name == "set-cookie") continue;
            e.headers.emplace_back(name, v);
        }
        size_t i = 0;
        while (i < vary.size()) {
            size_t comma = vary.find(',', i);
            if (comma == std::string::npos) comma = vary.size();
            const std::string name = lower(trim(vary.substr(i, comma - i)));
            if (!name.empty()) e.vary.emplace_back(name, header_of(req.headers, name));
            i = comma + 1;
        }
        store = cache_->put(e);
        if (store) count(&HttpCacheStats::stored);
    } else if (stored) {
        cache_->remove(req.url);  // replaced by something not to keep
    }
    mark(&r, "miss");
    return r;
}

}  // namespace wnk::net
