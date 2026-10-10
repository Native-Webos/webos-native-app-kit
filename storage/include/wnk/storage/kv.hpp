// SPDX-License-Identifier: GPL-3.0-or-later
//
// Durable key/value storage for application state (settings, sign-in state, small documents).
//
// Backends:
//   * Db8Store  — webOS: DB8 called from the app process over Luna (ADR-010). On an OLED65C1PTB,
//                 webOS 6.5.3, data survived relaunch, reinstall and a full reboot. Requires
//                 "requiredPermissions": ["database.operation"] in appinfo.json.
//   * FileStore — host, and disposable caches on the TV: one file per key, atomic replace.
//
// Semantics shared by every backend (tests/unit/test_storage.cpp runs one contract suite on both):
//   * keys: 1..64 chars of [a-z0-9._-], must start with a letter or digit;
//   * values: opaque bytes, at most `max_value_bytes` (default 64 KiB); callers serialise themselves;
//   * every stored entry has a revision; put()/remove() may pass the revision they expect and get
//     Status::Conflict if the entry changed meanwhile (kAnyRevision skips the check, kMustNotExist
//     only creates).
//
// Not a secret store: values are readable by anything that can read the app's DB8 kind or files.
// Calls block (a Luna round trip on the TV). Use them at startup or from a worker thread, never in
// the render loop. Each store serialises its own calls with a mutex.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace wnk::storage {

enum class Status {
    Ok,
    NotFound,
    Conflict,         // expected revision did not match, or kMustNotExist and the key exists
    InvalidKey,
    TooLarge,
    Unavailable,      // backend cannot be reached (no DB8, timeout, missing directory)
    Corrupt,          // stored data cannot be decoded
    IoError,
};

const char *status_name(Status s);

inline constexpr uint64_t kAnyRevision = UINT64_MAX;
inline constexpr uint64_t kMustNotExist = 0;  // real revisions are never 0

struct Entry {
    std::string value;
    uint64_t revision = 0;
};

struct PutResult {
    Status status = Status::IoError;
    uint64_t revision = 0;  // new revision when status == Ok
};

bool is_valid_key(std::string_view key);

class KeyValueStore {
 public:
    virtual ~KeyValueStore() = default;

    virtual Status get(std::string_view key, Entry *out) = 0;
    virtual PutResult put(std::string_view key, std::string_view value, uint64_t expected_revision = kAnyRevision) = 0;
    virtual Status remove(std::string_view key, uint64_t expected_revision = kAnyRevision) = 0;
    virtual const char *backend_name() const = 0;
    size_t max_value_bytes() const { return max_value_bytes_; }

 protected:
    explicit KeyValueStore(size_t max_value_bytes) : max_value_bytes_(max_value_bytes) {}
    size_t max_value_bytes_;
};

inline constexpr size_t kDefaultMaxValueBytes = 64 * 1024;

// Files under `directory` (must exist; created by the caller). Not safe for two processes at once.
std::unique_ptr<KeyValueStore> open_file_store(const std::string &directory,
                                               size_t max_value_bytes = kDefaultMaxValueBytes);

// Transport for Db8Store. The production implementation calls Luna; tests inject a fake DB8.
class Db8Transport {
 public:
    virtual ~Db8Transport() = default;
    // Calls luna://com.palm.db/<method> with a JSON payload. Returns false if the bus call itself
    // failed (no reply, timeout); otherwise fills `reply` with the JSON reply text.
    virtual bool call(const std::string &method, const std::string &payload, std::string *reply) = 0;
};

std::unique_ptr<Db8Transport> luna_db8_transport(int timeout_ms = 3000);

// DB8 store in kind "<app_id>.kv:1", owned by `app_id`. Registers the kind on first use.
std::unique_ptr<KeyValueStore> open_db8_store(const std::string &app_id, std::unique_ptr<Db8Transport> transport,
                                              size_t max_value_bytes = kDefaultMaxValueBytes);

// The platform default: DB8 on webOS, a FileStore under $XDG_DATA_HOME/<app_id>/kv (created) on the
// host. Returns nullptr with *error set if no durable store is available.
std::unique_ptr<KeyValueStore> open_default_store(const std::string &app_id, std::string *error = nullptr);

}  // namespace wnk::storage
