// SPDX-License-Identifier: GPL-3.0-or-later
//
// storage::BlobStore on a FileStore: large binary values in chunks, replacement without mixtures,
// interrupted writes keeping the old value, corruption detected, limits.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/storage/blob.hpp"

#include <cstdlib>
#include <random>
#include <string>

#include <unistd.h>

using namespace wnk::storage;

namespace {

struct Dir {
    std::string path;
    Dir() {
        char t[] = "/tmp/wnk-blob-XXXXXX";
        path = mkdtemp(t);
    }
    ~Dir() { std::system(("rm -rf '" + path + "'").c_str()); }
};

std::string random_bytes(size_t n, unsigned seed) {
    std::mt19937 g(seed);
    std::string s(n, '\0');
    for (char &c : s) c = static_cast<char>(g());
    return s;
}

// Passes calls through until `fail_after` puts, then refuses every put (a full disk, a lost bus).
struct FailingStore : KeyValueStore {
    KeyValueStore &inner;
    int fail_after;
    int puts = 0;
    FailingStore(KeyValueStore &kv, int n) : KeyValueStore(kv.max_value_bytes()), inner(kv), fail_after(n) {}
    Status get(std::string_view k, Entry *e) override { return inner.get(k, e); }
    PutResult put(std::string_view k, std::string_view v, uint64_t rev) override {
        if (++puts > fail_after) return {Status::IoError, 0};
        return inner.put(k, v, rev);
    }
    Status remove(std::string_view k, uint64_t rev) override { return inner.remove(k, rev); }
    const char *backend_name() const override { return "failing"; }
};

}  // namespace

TEST("blob: a megabyte of binary round-trips in chunks; replacing it removes the old chunks") {
    Dir dir;
    auto kv = open_file_store(dir.path);
    REQUIRE(kv != nullptr);
    BlobStore blobs(*kv);
    const std::string big = random_bytes(1u << 20, 1);
    CHECK(blobs.put("save-1", big) == Status::Ok);
    std::string out;
    CHECK(blobs.get("save-1", &out) == Status::Ok);
    CHECK(out == big);
    CHECK(blobs.size("save-1") == std::optional<size_t>(big.size()));
    Entry e;
    CHECK(kv->get("save-1.1.31", &e) == Status::Ok);  // 32 chunks of 32 KiB
    CHECK(kv->get("save-1.1.32", &e) == Status::NotFound);
    const std::string small = "level=3";
    CHECK(blobs.put("save-1", small) == Status::Ok);
    CHECK(blobs.get("save-1", &out) == Status::Ok);
    CHECK_EQ(out, small);
    CHECK(kv->get("save-1.1.0", &e) == Status::NotFound);  // the first generation is gone
    CHECK(kv->get("save-1.2.0", &e) == Status::Ok);
    CHECK(blobs.put("empty", "") == Status::Ok);
    CHECK(blobs.get("empty", &out) == Status::Ok);
    CHECK(out.empty());
}

TEST("blob: an interrupted write leaves the previous value whole, and the next write cleans up") {
    Dir dir;
    auto kv = open_file_store(dir.path);
    BlobStore good(*kv);
    const std::string v1 = random_bytes(100000, 2), v2 = random_bytes(200000, 3);
    REQUIRE(good.put("game", v1) == Status::Ok);
    FailingStore failing(*kv, 3);  // three chunks of the new value, then the store fails
    BlobStore bad(failing);
    CHECK(bad.put("game", v2) == Status::IoError);
    std::string out;
    CHECK(good.get("game", &out) == Status::Ok);
    CHECK(out == v1);
    Entry e;
    CHECK(kv->get("game.2.0", &e) == Status::NotFound);  // the partial generation was removed
    CHECK(good.put("game", v2) == Status::Ok);
    CHECK(good.get("game", &out) == Status::Ok);
    CHECK(out == v2);
}

TEST("blob: missing or damaged chunks are Corrupt; names, limits and removal") {
    Dir dir;
    auto kv = open_file_store(dir.path);
    BlobStore blobs(*kv, 100000);
    const std::string v = random_bytes(70000, 4);
    REQUIRE(blobs.put("cache", v) == Status::Ok);
    std::string out;
    REQUIRE(kv->put("cache.1.1", "tampered").status == Status::Ok);
    CHECK(blobs.get("cache", &out) == Status::Corrupt);
    REQUIRE(blobs.put("cache", v) == Status::Ok);
    REQUIRE(kv->remove("cache.2.2") == Status::Ok);
    CHECK(blobs.get("cache", &out) == Status::Corrupt);
    REQUIRE(kv->put("cache.m", R"({"gen":"x"})").status == Status::Ok);
    CHECK(blobs.get("cache", &out) == Status::Corrupt);
    CHECK(blobs.put("cache", v) == Status::Ok);  // a damaged manifest is replaced
    CHECK(blobs.put("cache", std::string(100001, 'x')) == Status::TooLarge);
    CHECK(blobs.put("Bad Name", "x") == Status::InvalidKey);
    CHECK(blobs.put(std::string(41, 'a'), "x") == Status::InvalidKey);
    CHECK(blobs.remove("cache") == Status::Ok);
    CHECK(blobs.get("cache", &out) == Status::NotFound);
    Entry e;
    CHECK(kv->get("cache.4.0", &e) == Status::NotFound);
    CHECK(blobs.remove("cache") == Status::NotFound);
    CHECK(crc32("123456789") == 0xCBF43926u);  // the standard check value
}
