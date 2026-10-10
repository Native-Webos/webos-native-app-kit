// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/storage/kv.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <functional>
#include <map>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace wnk::storage;
using json = nlohmann::json;

namespace {

// In-memory DB8 with the behaviour measured on an OLED65C1PTB, webOS 6.5.3 (2026-10-11):
// custom _id too long → -3968 "db: Invalid _id length"; update without _rev → -3960
// "db: _rev must be specified for existing objects". A stale _rev error text is assumed.
struct FakeDb8 {
    std::map<std::string, std::string> kinds;  // kind -> owner
    std::map<std::string, json> objects;       // _id -> object
    uint64_t next_rev = 1000;
    int next_id = 1;
    int putkind_calls = 0;
    bool offline = false;
    std::string forced_error;                  // reply with this errorText to every call
    std::function<void()> before_put;          // simulate another writer racing us

    static std::string error(int code, const std::string &text) {
        return json{{"returnValue", false}, {"errorCode", code}, {"errorText", text}}.dump();
    }

    bool handle(const std::string &method, const std::string &payload, std::string *reply) {
        if (offline) return false;
        if (!forced_error.empty()) {
            *reply = error(-1, forced_error);
            return true;
        }
        const json p = json::parse(payload);
        if (method == "putKind") {
            ++putkind_calls;
            const std::string id = p.at("id"), owner = p.at("owner");
            auto it = kinds.find(id);
            if (it != kinds.end() && it->second != owner) {
                *reply = error(-3999, "db: permission denied");
                return true;
            }
            kinds[id] = owner;
            *reply = R"({"returnValue":true})";
        } else if (method == "put") {
            if (before_put) {
                auto hook = before_put;
                before_put = nullptr;
                hook();
            }
            json results = json::array();
            for (auto obj : p.at("objects")) {
                if (!kinds.count(obj.value("_kind", ""))) {
                    *reply = error(-3970, "db: kind not registered");
                    return true;
                }
                std::string id;
                if (obj.contains("_id")) {
                    id = obj["_id"];
                    if (id.size() > 20) {
                        *reply = error(-3968, "db: Invalid _id length");
                        return true;
                    }
                    auto it = objects.find(id);
                    if (it != objects.end()) {
                        if (!obj.contains("_rev")) {
                            *reply = error(-3960, "db: _rev must be specified for existing objects");
                            return true;
                        }
                        if (obj["_rev"] != it->second["_rev"]) {
                            *reply = error(-3961, "db: revision mismatch");
                            return true;
                        }
                    }
                } else {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "++Fake%06d", next_id++);
                    id = buf;
                }
                obj["_id"] = id;
                obj["_rev"] = ++next_rev;
                objects[id] = obj;
                results.push_back({{"id", id}, {"rev", next_rev}});
            }
            *reply = json{{"returnValue", true}, {"results", results}}.dump();
        } else if (method == "find") {
            const auto &q = p.at("query");
            const std::string kind = q.at("from");
            const auto &w = q.at("where").at(0);
            json results = json::array();
            for (const auto &[id, obj] : objects) {
                if (obj["_kind"] == kind && obj.value(w.at("prop").get<std::string>(), json()) == w.at("val")) {
                    results.push_back(obj);
                }
            }
            *reply = json{{"returnValue", true}, {"results", results}}.dump();
        } else if (method == "del") {
            json results = json::array();
            for (const auto &id : p.at("ids")) {
                objects.erase(id.get<std::string>());
                results.push_back({{"id", id}});
            }
            *reply = json{{"returnValue", true}, {"results", results}}.dump();
        } else {
            *reply = error(-1, "unknown method");
        }
        return true;
    }
};

struct FakeTransport final : Db8Transport {
    FakeDb8 *db;
    explicit FakeTransport(FakeDb8 *d) : db(d) {}
    bool call(const std::string &method, const std::string &payload, std::string *reply) override {
        return db->handle(method, payload, reply);
    }
};

std::string temp_dir() {
    char tmpl[] = "/tmp/wnk-kv-XXXXXX";
    const char *d = mkdtemp(tmpl);
    return d != nullptr ? std::string(d) : std::string();
}

void remove_tree(const std::string &dir) {
    std::string cmd = "rm -rf '" + dir + "'";
    if (dir.rfind("/tmp/wnk-kv-", 0) == 0) (void)std::system(cmd.c_str());
}

// ---- contract suite, run against both backends ----

void contract(KeyValueStore &kv) {
    Entry e;
    CHECK(kv.get("missing", &e) == Status::NotFound);

    PutResult a = kv.put("app.settings", R"({"volume":3})");
    REQUIRE(a.status == Status::Ok);
    CHECK(a.revision > 0);
    REQUIRE(kv.get("app.settings", &e) == Status::Ok);
    CHECK_EQ(e.value, std::string(R"({"volume":3})"));
    CHECK_EQ(e.revision, a.revision);

    PutResult b = kv.put("app.settings", "v2");
    REQUIRE(b.status == Status::Ok);
    CHECK(b.revision > a.revision);

    CHECK(kv.put("app.settings", "stale", a.revision).status == Status::Conflict);
    CHECK(kv.put("app.settings", "fresh", b.revision).status == Status::Ok);
    REQUIRE(kv.get("app.settings", &e) == Status::Ok);
    CHECK_EQ(e.value, std::string("fresh"));

    CHECK(kv.put("only-once", "1", kMustNotExist).status == Status::Ok);
    CHECK(kv.put("only-once", "2", kMustNotExist).status == Status::Conflict);
    CHECK(kv.put("never-written", "x", 12345).status == Status::Conflict);

    REQUIRE(kv.get("only-once", &e) == Status::Ok);
    CHECK(kv.remove("only-once", e.revision + 1) == Status::Conflict);
    CHECK(kv.remove("only-once", e.revision) == Status::Ok);
    CHECK(kv.get("only-once", &e) == Status::NotFound);
    CHECK(kv.remove("only-once") == Status::NotFound);

    for (const char *bad : {"", "Upper", ".hidden", "a/b", "sp ace", "-dash"}) {
        CHECK(kv.put(bad, "x").status == Status::InvalidKey);
        CHECK(kv.get(bad, &e) == Status::InvalidKey);
    }
    CHECK(kv.put(std::string(65, 'a'), "x").status == Status::InvalidKey);
    CHECK(kv.put(std::string(64, 'a'), "x").status == Status::Ok);  // well past DB8's _id limit

    CHECK(kv.put("big", std::string(kv.max_value_bytes() + 1, 'x')).status == Status::TooLarge);
    CHECK(kv.put("big", std::string(kv.max_value_bytes(), 'x')).status == Status::Ok);

    const std::string binary("\x00\xff\xfe\n\"\\\x80zz", 10);
    REQUIRE(kv.put("bin", binary).status == Status::Ok);
    REQUIRE(kv.get("bin", &e) == Status::Ok);
    CHECK(e.value == binary);

    REQUIRE(kv.put("unicode", "Grüße · 日本 · 😀").status == Status::Ok);
    REQUIRE(kv.get("unicode", &e) == Status::Ok);
    CHECK_EQ(e.value, std::string("Grüße · 日本 · 😀"));

    REQUIRE(kv.put("empty", "").status == Status::Ok);
    REQUIRE(kv.get("empty", &e) == Status::Ok);
    CHECK(e.value.empty());

    for (int i = 0; i < 20; ++i) CHECK(kv.put("k" + std::to_string(i), std::to_string(i * i)).status == Status::Ok);
    for (int i = 0; i < 20; ++i) {
        REQUIRE(kv.get("k" + std::to_string(i), &e) == Status::Ok);
        CHECK_EQ(e.value, std::to_string(i * i));
    }
}

}  // namespace

TEST("key validation") {
    CHECK(is_valid_key("a"));
    CHECK(is_valid_key("session.token-v2_1"));
    CHECK(!is_valid_key(""));
    CHECK(!is_valid_key("A"));
    CHECK(!is_valid_key("_x"));
    CHECK(!is_valid_key(std::string(65, 'a')));
}

TEST("file store satisfies the contract") {
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    auto kv = open_file_store(dir);
    contract(*kv);
    CHECK(std::string(kv->backend_name()) == "file");
    remove_tree(dir);
}

TEST("db8 store satisfies the contract against the fake DB8") {
    FakeDb8 db;
    auto kv = open_db8_store("org.example.app", std::make_unique<FakeTransport>(&db));
    contract(*kv);
    CHECK(std::string(kv->backend_name()) == "db8");
    CHECK_EQ(db.putkind_calls, 1);  // kind registered once per store
    for (const auto &[id, obj] : db.objects) CHECK(id.rfind("++Fake", 0) == 0);  // DB8 assigns every _id
    CHECK_EQ(db.kinds["org.example.app.kv:1"], std::string("org.example.app"));
}

TEST("file store persists across instances and leaves no temp files") {
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    uint64_t rev = 0;
    {
        auto kv = open_file_store(dir);
        rev = kv->put("persist", "hello").revision;
    }
    auto kv = open_file_store(dir);
    Entry e;
    REQUIRE(kv->get("persist", &e) == Status::Ok);
    CHECK_EQ(e.value, std::string("hello"));
    CHECK_EQ(e.revision, rev);
    CHECK(access((dir + "/persist.kv.tmp").c_str(), F_OK) != 0);
    struct stat st{};
    REQUIRE(stat((dir + "/persist.kv").c_str(), &st) == 0);
    CHECK_EQ(st.st_mode & 0777, 0600u);
    remove_tree(dir);
}

TEST("file store detects corruption and refuses symlinks") {
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    auto kv = open_file_store(dir);
    REQUIRE(kv->put("doc", "important data").status == Status::Ok);
    {
        const int fd = ::open((dir + "/doc.kv").c_str(), O_WRONLY);
        REQUIRE(fd >= 0);
        ::lseek(fd, -3, SEEK_END);
        REQUIRE(::write(fd, "XXX", 3) == 3);
        ::close(fd);
    }
    Entry e;
    CHECK(kv->get("doc", &e) == Status::Corrupt);
    CHECK(kv->put("doc", "x").status == Status::Corrupt);  // never silently overwrite unreadable data
    REQUIRE(::symlink("/etc/hostname", (dir + "/link.kv").c_str()) == 0);
    CHECK(kv->get("link", &e) == Status::Corrupt);
    remove_tree(dir);
}

TEST("file store in a missing directory is unavailable") {
    auto kv = open_file_store("/tmp/wnk-kv-does-not-exist/nested");
    Entry e;
    CHECK(kv->get("x", &e) == Status::Unavailable);
    CHECK(kv->put("x", "y").status == Status::Unavailable);
}

TEST("db8 store reports transport and permission failures as unavailable") {
    FakeDb8 db;
    auto kv = open_db8_store("org.example.app", std::make_unique<FakeTransport>(&db));
    db.offline = true;
    Entry e;
    CHECK(kv->get("x", &e) == Status::Unavailable);
    db.offline = false;
    db.forced_error = "db: permission denied";
    CHECK(kv->put("x", "y").status == Status::Unavailable);
    db.forced_error.clear();
    CHECK(kv->put("x", "y").status == Status::Ok);
}

TEST("db8 store turns a concurrent writer into a conflict") {
    FakeDb8 db;
    auto kv = open_db8_store("org.example.app", std::make_unique<FakeTransport>(&db));
    REQUIRE(kv->put("shared", "v1").status == Status::Ok);
    db.before_put = [&] {
        for (auto &[id, obj] : db.objects) obj["_rev"] = ++db.next_rev;  // someone else wrote first
    };
    CHECK(kv->put("shared", "mine").status == Status::Conflict);
    Entry e;
    REQUIRE(kv->get("shared", &e) == Status::Ok);
    CHECK_EQ(e.value, std::string("v1"));
}

TEST("db8 store flags duplicate records and malformed replies as corrupt") {
    FakeDb8 db;
    auto kv = open_db8_store("org.example.app", std::make_unique<FakeTransport>(&db));
    REQUIRE(kv->put("dup", "a").status == Status::Ok);
    json copy = db.objects.begin()->second;
    copy["_id"] = "++FakeCopy";
    db.objects["++FakeCopy"] = copy;
    Entry e;
    CHECK(kv->get("dup", &e) == Status::Corrupt);
    db.objects.clear();
    db.objects["++Bad"] = json{{"_id", "++Bad"}, {"_kind", "org.example.app.kv:1"}, {"_rev", 5}, {"key", "bad"}};
    CHECK(kv->get("bad", &e) == Status::Corrupt);  // neither value nor value_b64
}

TEST("db8 store refuses a kind owned by someone else") {
    FakeDb8 db;
    db.kinds["org.example.app.kv:1"] = "com.other.app";
    auto kv = open_db8_store("org.example.app", std::make_unique<FakeTransport>(&db));
    Entry e;
    CHECK(kv->get("x", &e) == Status::Unavailable);
}

TEST("default store on the host is a file store under XDG_DATA_HOME") {
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    setenv("XDG_DATA_HOME", dir.c_str(), 1);
    std::string err;
    auto kv = open_default_store("org.example.app", &err);
    REQUIRE(kv != nullptr);
    CHECK(std::string(kv->backend_name()) == "file");
    CHECK(kv->put("x", "1").status == Status::Ok);
    struct stat st{};
    CHECK(stat((dir + "/org.example.app/kv/x.kv").c_str(), &st) == 0);
    remove_tree(dir);
}
