// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/storage/kv.hpp"

#include "wnk/platform/luna.hpp"
#include "wnk/platform/process.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wnk::storage {

using json = nlohmann::json;

const char *status_name(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::NotFound: return "not-found";
        case Status::Conflict: return "conflict";
        case Status::InvalidKey: return "invalid-key";
        case Status::TooLarge: return "too-large";
        case Status::Unavailable: return "unavailable";
        case Status::Corrupt: return "corrupt";
        case Status::IoError: return "io-error";
    }
    return "?";
}

bool is_valid_key(std::string_view key) {
    if (key.empty() || key.size() > 64) return false;
    for (size_t i = 0; i < key.size(); ++i) {
        const char c = key[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (i == 0 ? !alnum : !(alnum || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

namespace {

// ---- helpers ----------------------------------------------------------------------------------

uint32_t crc32(std::string_view data) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
        if (len == 0 || i + static_cast<size_t>(len) > s.size()) return false;
        uint32_t cp = len == 1 ? b : b & (0x7F >> len);
        for (int k = 1; k < len; ++k) {
            const auto c = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3F);
        }
        static constexpr uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += static_cast<size_t>(len);
    }
    return true;
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::string_view in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        const uint32_t v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
        i += 3;
    }
    if (i < in.size()) {
        uint32_t v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool base64_decode(std::string_view in, std::string *out) {
    if (in.size() % 4 != 0) return false;
    out->clear();
    auto val = [](char c) -> int {
        const char *p = std::strchr(kB64, c);
        return (c != '\0' && p != nullptr) ? static_cast<int>(p - kB64) : -1;
    };
    for (size_t i = 0; i < in.size(); i += 4) {
        const int a = val(in[i]), b = val(in[i + 1]);
        const bool pad3 = in[i + 2] == '=', pad4 = in[i + 3] == '=';
        const int c = pad3 ? 0 : val(in[i + 2]), d = pad4 ? 0 : val(in[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad3 && !pad4) || ((pad3 || pad4) && i + 4 != in.size())) return false;
        const uint32_t v = (static_cast<uint32_t>(a) << 18) | (static_cast<uint32_t>(b) << 12) |
                           (static_cast<uint32_t>(c) << 6) | static_cast<uint32_t>(d);
        out->push_back(static_cast<char>((v >> 16) & 0xFF));
        if (!pad3) out->push_back(static_cast<char>((v >> 8) & 0xFF));
        if (!pad4) out->push_back(static_cast<char>(v & 0xFF));
    }
    return true;
}

bool write_all(int fd, const char *data, size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

// ---- FileStore ----------------------------------------------------------------------------------
//
// File format, "<key>.kv": one header line "WNKKV1 <revision> <length> <crc32-hex>\n", then the value
// bytes. Replaced atomically: write "<key>.kv.tmp", fsync, rename over the target, fsync the directory.

class FileStore final : public KeyValueStore {
 public:
    FileStore(std::string dir, size_t max) : KeyValueStore(max), dir_(std::move(dir)) {}

    const char *backend_name() const override { return "file"; }

    Status get(std::string_view key, Entry *out) override {
        if (!is_valid_key(key)) return Status::InvalidKey;
        std::lock_guard<std::mutex> lock(mutex_);
        return read(key, out);
    }

    PutResult put(std::string_view key, std::string_view value, uint64_t expected) override {
        if (!is_valid_key(key)) return {Status::InvalidKey, 0};
        if (value.size() > max_value_bytes_) return {Status::TooLarge, 0};
        std::lock_guard<std::mutex> lock(mutex_);
        Entry current;
        const Status s = read(key, &current);
        if (s != Status::Ok && s != Status::NotFound) return {s, 0};
        const bool exists = s == Status::Ok;
        if (expected == kMustNotExist && exists) return {Status::Conflict, 0};
        if (expected != kAnyRevision && expected != kMustNotExist && (!exists || current.revision != expected)) {
            return {Status::Conflict, 0};
        }
        const uint64_t rev = exists ? current.revision + 1 : 1;
        char header[96];
        std::snprintf(header, sizeof header, "WNKKV1 %llu %zu %08x\n", static_cast<unsigned long long>(rev),
                      value.size(), crc32(value));
        const std::string target = path(key);
        const std::string tmp = target + ".tmp";
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return {errno == ENOENT ? Status::Unavailable : Status::IoError, 0};
        const bool ok = write_all(fd, header, std::strlen(header)) && write_all(fd, value.data(), value.size()) &&
                        ::fsync(fd) == 0;
        ::close(fd);
        if (!ok || ::rename(tmp.c_str(), target.c_str()) != 0) {
            ::unlink(tmp.c_str());
            return {Status::IoError, 0};
        }
        sync_dir();
        return {Status::Ok, rev};
    }

    Status remove(std::string_view key, uint64_t expected) override {
        if (!is_valid_key(key)) return Status::InvalidKey;
        std::lock_guard<std::mutex> lock(mutex_);
        Entry current;
        const Status s = read(key, &current);
        if (s != Status::Ok) return s;
        if (expected != kAnyRevision && current.revision != expected) return Status::Conflict;
        if (::unlink(path(key).c_str()) != 0) return Status::IoError;
        sync_dir();
        return Status::Ok;
    }

 private:
    std::string path(std::string_view key) const { return dir_ + "/" + std::string(key) + ".kv"; }

    void sync_dir() const {
        const int d = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (d >= 0) {
            ::fsync(d);
            ::close(d);
        }
    }

    Status read(std::string_view key, Entry *out) const {
        const int fd = ::open(path(key).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT) {
                struct stat st{};
                return ::stat(dir_.c_str(), &st) == 0 ? Status::NotFound : Status::Unavailable;
            }
            return errno == ELOOP ? Status::Corrupt : Status::IoError;
        }
        std::string data;
        char buf[16384];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof buf)) > 0) {
            data.append(buf, static_cast<size_t>(n));
            if (data.size() > max_value_bytes_ + 128) break;
        }
        ::close(fd);
        if (n < 0) return Status::IoError;
        const size_t nl = data.find('\n');
        unsigned long long rev = 0;
        size_t len = 0;
        unsigned int crc = 0;
        char magic[8] = {};
        if (nl == std::string::npos ||
            std::sscanf(data.substr(0, nl).c_str(), "%7s %llu %zu %8x", magic, &rev, &len, &crc) != 4 ||
            std::strcmp(magic, "WNKKV1") != 0 || rev == 0 || data.size() - nl - 1 != len) {
            return Status::Corrupt;
        }
        std::string_view value(data.data() + nl + 1, len);
        if (crc32(value) != crc) return Status::Corrupt;
        if (out != nullptr) {
            out->value.assign(value.data(), value.size());
            out->revision = rev;
        }
        return Status::Ok;
    }

    std::string dir_;
    std::mutex mutex_;
};

// ---- Db8Store -----------------------------------------------------------------------------------
//
// One DB8 kind "<app_id>.kv:1" owned by the app, with an index on "key". Records are found by key, so
// DB8 assigns the _id (custom _ids are length-limited: 33 characters were rejected on webOS 6.5.3).
// The entry revision is DB8's _rev; updates pass it, which DB8 requires for existing objects.
// Values are stored as {"value": "<utf-8>"} or, when not valid UTF-8, {"value_b64": "<base64>"}.

class Db8Store final : public KeyValueStore {
 public:
    Db8Store(std::string app_id, std::unique_ptr<Db8Transport> t, size_t max)
        : KeyValueStore(max), app_id_(std::move(app_id)), kind_(app_id_ + ".kv:1"), transport_(std::move(t)) {}

    const char *backend_name() const override { return "db8"; }

    Status get(std::string_view key, Entry *out) override {
        if (!is_valid_key(key)) return Status::InvalidKey;
        std::lock_guard<std::mutex> lock(mutex_);
        json obj;
        const Status s = find(key, &obj);
        if (s != Status::Ok) return s;
        return decode(obj, out);
    }

    PutResult put(std::string_view key, std::string_view value, uint64_t expected) override {
        if (!is_valid_key(key)) return {Status::InvalidKey, 0};
        if (value.size() > max_value_bytes_) return {Status::TooLarge, 0};
        std::lock_guard<std::mutex> lock(mutex_);
        json existing;
        const Status s = find(key, &existing);
        if (s != Status::Ok && s != Status::NotFound) return {s, 0};
        const bool exists = s == Status::Ok;
        const uint64_t current = exists ? existing.value("_rev", uint64_t{0}) : 0;
        if (expected == kMustNotExist && exists) return {Status::Conflict, 0};
        if (expected != kAnyRevision && expected != kMustNotExist && (!exists || current != expected)) {
            return {Status::Conflict, 0};
        }
        json object = {{"_kind", kind_}, {"key", std::string(key)}};
        if (valid_utf8(value)) {
            object["value"] = std::string(value);
        } else {
            object["value_b64"] = base64_encode(value);
        }
        if (exists) {
            object["_id"] = existing["_id"];
            object["_rev"] = current;
        }
        json reply;
        const Status c = call("put", json{{"objects", json::array({object})}}, &reply);
        if (c != Status::Ok) return {c, 0};
        const auto &results = reply["results"];
        if (!results.is_array() || results.size() != 1 || !results[0].is_object() ||
            !results[0].contains("rev") || !results[0]["rev"].is_number_unsigned()) {
            return {Status::Corrupt, 0};
        }
        return {Status::Ok, results[0]["rev"].get<uint64_t>()};
    }

    Status remove(std::string_view key, uint64_t expected) override {
        if (!is_valid_key(key)) return Status::InvalidKey;
        std::lock_guard<std::mutex> lock(mutex_);
        json existing;
        const Status s = find(key, &existing);
        if (s != Status::Ok) return s;
        if (expected != kAnyRevision && existing.value("_rev", uint64_t{0}) != expected) return Status::Conflict;
        json reply;
        return call("del", json{{"ids", json::array({existing["_id"]})}}, &reply);
    }

 private:
    // Sends one DB8 call. Ok only when the bus call succeeded and DB8 answered returnValue:true.
    Status call(const std::string &method, const json &payload, json *reply) {
        std::string text;
        if (!transport_->call(method, payload.dump(), &text)) return Status::Unavailable;
        json r = json::parse(text, nullptr, false);
        if (r.is_discarded() || !r.is_object()) return Status::Corrupt;
        if (r.value("returnValue", false)) {
            *reply = std::move(r);
            return Status::Ok;
        }
        const std::string err = r.value("errorText", std::string());
        // DB8 refuses a stale or missing _rev; we always send the one we just read, so a mismatch
        // means another writer changed the record in between.
        if (err.find("_rev") != std::string::npos || err.find("revision") != std::string::npos) return Status::Conflict;
        if (err.find("permission") != std::string::npos || err.find("denied") != std::string::npos) {
            return Status::Unavailable;
        }
        return Status::IoError;
    }

    Status ensure_kind() {
        if (kind_ready_) return Status::Ok;
        const json payload = {{"id", kind_},
                              {"owner", app_id_},
                              {"indexes", json::array({{{"name", "key"}, {"props", json::array({{{"name", "key"}}})}}})}};
        json reply;
        const Status s = call("putKind", payload, &reply);
        if (s == Status::Ok) kind_ready_ = true;
        return s;
    }

    Status find(std::string_view key, json *obj) {
        const Status k = ensure_kind();
        if (k != Status::Ok) return k;
        const json payload = {
            {"query",
             {{"from", kind_},
              {"where", json::array({{{"prop", "key"}, {"op", "="}, {"val", std::string(key)}}})},
              {"limit", 2}}}};
        json reply;
        const Status s = call("find", payload, &reply);
        if (s != Status::Ok) return s;
        const auto &results = reply["results"];
        if (!results.is_array()) return Status::Corrupt;
        if (results.empty()) return Status::NotFound;
        if (results.size() > 1 || !results[0].is_object() || !results[0].contains("_id")) return Status::Corrupt;
        *obj = results[0];
        return Status::Ok;
    }

    static Status decode(const json &obj, Entry *out) {
        if (!obj.contains("_rev") || !obj["_rev"].is_number_unsigned()) return Status::Corrupt;
        const uint64_t rev = obj["_rev"].get<uint64_t>();
        if (rev == 0) return Status::Corrupt;
        std::string value;
        if (obj.contains("value") && obj["value"].is_string()) {
            value = obj["value"].get<std::string>();
        } else if (obj.contains("value_b64") && obj["value_b64"].is_string()) {
            if (!base64_decode(obj["value_b64"].get<std::string>(), &value)) return Status::Corrupt;
        } else {
            return Status::Corrupt;
        }
        if (out != nullptr) {
            out->value = std::move(value);
            out->revision = rev;
        }
        return Status::Ok;
    }

    std::string app_id_;
    std::string kind_;
    std::unique_ptr<Db8Transport> transport_;
    std::mutex mutex_;
    bool kind_ready_ = false;
};

class LunaDb8Transport final : public Db8Transport {
 public:
    explicit LunaDb8Transport(int timeout_ms) : timeout_ms_(timeout_ms) {}
    bool call(const std::string &method, const std::string &payload, std::string *reply) override {
        const auto r = platform::luna_call_sync("luna://com.palm.db/" + method, payload, timeout_ms_);
        if (r.status != platform::LunaStatus::Ok) return false;
        *reply = r.payload;
        return true;
    }

 private:
    int timeout_ms_;
};

bool make_dirs(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || (path[i] == '/' && i != 0)) {
            cur = path.substr(0, i);
            if (::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
    }
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace

std::unique_ptr<KeyValueStore> open_file_store(const std::string &directory, size_t max_value_bytes) {
    return std::make_unique<FileStore>(directory, max_value_bytes);
}

std::unique_ptr<Db8Transport> luna_db8_transport(int timeout_ms) {
    return std::make_unique<LunaDb8Transport>(timeout_ms);
}

std::unique_ptr<KeyValueStore> open_db8_store(const std::string &app_id, std::unique_ptr<Db8Transport> transport,
                                              size_t max_value_bytes) {
    return std::make_unique<Db8Store>(app_id, std::move(transport), max_value_bytes);
}

std::unique_ptr<KeyValueStore> open_default_store(const std::string &app_id, std::string *error) {
    if (platform::kIsWebOS) {
        auto store = open_db8_store(app_id, luna_db8_transport());
        // Probe once so a missing permission or DB8 problem is reported at startup, not on first save.
        const Status s = store->get("wnk.probe", nullptr);
        if (s == Status::Ok || s == Status::NotFound) return store;
        if (error != nullptr) {
            *error = std::string("DB8 store unavailable (") + status_name(s) +
                     "); is \"database.operation\" in appinfo.json requiredPermissions?";
        }
        return nullptr;
    }
    const char *xdg = std::getenv("XDG_DATA_HOME");
    const char *home = std::getenv("HOME");
    std::string base = (xdg != nullptr && xdg[0] == '/') ? std::string(xdg)
                       : (home != nullptr && home[0] == '/') ? std::string(home) + "/.local/share"
                                                             : std::string();
    if (base.empty() || app_id.empty() || app_id.find('/') != std::string::npos) {
        if (error != nullptr) *error = "no data directory (set XDG_DATA_HOME or HOME)";
        return nullptr;
    }
    const std::string dir = base + "/" + app_id + "/kv";
    if (!make_dirs(dir)) {
        if (error != nullptr) *error = "cannot create " + dir + ": " + std::strerror(errno);
        return nullptr;
    }
    return open_file_store(dir);
}

}  // namespace wnk::storage
