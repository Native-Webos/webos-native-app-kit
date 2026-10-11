// SPDX-License-Identifier: GPL-3.0-or-later
//
// SecretStore against an in-memory KeyValueStore (a FileStore in a temp dir) and a fake keymanager3
// that follows the protocol PlxNative uses on the TV (generateKey / begin / finish / abort, -10002 for
// an existing key) and authenticates its "ciphertext", so tampering fails like AES-GCM would.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/storage/secret.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <map>
#include <string>

#include <unistd.h>

using namespace wnk::storage;
using json = nlohmann::json;

namespace {

struct FakeKeymanager : KeyService {
    enum class Mode { Working, Absent, Denied } mode = Mode::Working;
    std::map<std::string, std::string> keys;     // name → secret byte
    std::map<std::string, json> handles;         // handle → begin params
    int next = 1, aborts = 0, calls = 0;

    std::optional<std::string> call(const std::string &method, const std::string &payload) override {
        ++calls;
        if (mode == Mode::Absent) return std::nullopt;
        if (mode == Mode::Denied) return json{{"returnValue", false}, {"errorCode", -1}, {"errorText", "Denied method call"}}.dump();
        const json p = json::parse(payload);
        if (method == "generateKey") {
            const std::string name = p["name"];
            if (keys.count(name)) return json{{"returnValue", false}, {"errorCode", -10002}, {"errorText", "Key exists"}}.dump();
            keys[name] = "k" + std::to_string(keys.size());
            return json{{"returnValue", true}}.dump();
        }
        if (method == "begin") {
            if (!keys.count(p["name"].get<std::string>())) return json{{"returnValue", false}, {"errorCode", -10001}}.dump();
            const std::string h = "h" + std::to_string(next++);
            json st = p;
            json reply = {{"returnValue", true}, {"handle", h}};
            if (p["params"]["purpose"][0] == "encrypt") {
                st["iv"] = "iv" + std::to_string(next);
                reply["iv"] = st["iv"];
            } else {
                st["iv"] = p["params"]["iv"];
            }
            handles[h] = st;
            return reply.dump();
        }
        if (method == "finish") {
            auto it = handles.find(p["handle"]);
            if (it == handles.end()) return json{{"returnValue", false}, {"errorCode", -10003}}.dump();
            const json st = it->second;
            handles.erase(it);
            const std::string key = keys[st["name"].get<std::string>()];
            const std::string iv = st["iv"];
            if (st["params"]["purpose"][0] == "encrypt") {
                // "ciphertext" = tag|plain_b64, tag binds key, iv and data
                const std::string data = p["data"];
                return json{{"returnValue", true}, {"output", key + iv + "|" + data}}.dump();
            }
            const std::string ct = p["data"];
            const std::string prefix = key + iv + "|";
            if (ct.rfind(prefix, 0) != 0) return json{{"returnValue", false}, {"errorCode", -10010}, {"errorText", "auth failed"}}.dump();
            return json{{"returnValue", true}, {"output", ct.substr(prefix.size())}}.dump();
        }
        if (method == "abort") {
            ++aborts;
            handles.erase(p["handle"].get<std::string>());
            return json{{"returnValue", true}}.dump();
        }
        return std::nullopt;
    }
};

struct Rig {
    std::string dir;
    std::unique_ptr<KeyValueStore> kv;
    FakeKeymanager *km = nullptr;
    std::unique_ptr<SecretStore> secrets;
    explicit Rig(FakeKeymanager::Mode mode = FakeKeymanager::Mode::Working) {
        char tmpl[] = "/tmp/wnk-secret-XXXXXX";
        dir = mkdtemp(tmpl);
        kv = open_file_store(dir);
        reopen(mode);
    }
    void reopen(FakeKeymanager::Mode mode, bool keep_keys = true) {
        auto fake = std::make_unique<FakeKeymanager>();
        if (km != nullptr && keep_keys) fake->keys = km->keys;
        fake->mode = mode;
        km = fake.get();
        secrets = std::make_unique<SecretStore>(*kv, std::move(fake), "org.wnk.test.secrets.v1");
    }
    ~Rig() { std::system(("rm -rf '" + dir + "'").c_str()); }
    std::string raw(const std::string &name) {
        Entry e;
        kv->get("secret." + name, &e);
        return e.value;
    }
};

}  // namespace

TEST("base64: round trips, padding, rejects garbage") {
    const std::string cases[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar", std::string("\0\xff\x10", 3)};
    for (const std::string &s : cases) {
        const auto d = base64_decode(base64_encode(s));
        REQUIRE(d.has_value());
        CHECK(*d == s);
    }
    CHECK_EQ(base64_encode("foobar"), std::string("Zm9vYmFy"));
    CHECK_EQ(base64_encode("fo"), std::string("Zm8="));
    CHECK(!base64_decode("Zm9").has_value());
    CHECK(!base64_decode("Zm=v").has_value());
    CHECK(!base64_decode("Z!9v").has_value());
}

TEST("secrets: with keymanager3 a secret is sealed, never stored in the clear, and reads back") {
    Rig r;
    CHECK(r.secrets->protection() == Protection::Keymanager3);
    CHECK(r.secrets->put("server.token", "s3cr3t-token-value") == SecretStatus::Ok);
    const std::string stored = r.raw("server.token");
    CHECK(stored.find("keymanager3-aes256-gcm-v1") != std::string::npos);
    CHECK(stored.find("s3cr3t-token-value") == std::string::npos);
    CHECK(stored.find(base64_encode("s3cr3t-token-value")) == std::string::npos || stored.find("|") != std::string::npos);
    std::string out;
    CHECK(r.secrets->get("server.token", &out) == SecretStatus::Ok);
    CHECK_EQ(out, std::string("s3cr3t-token-value"));
    // A second store instance (next launch) finds the existing key (-10002) and still opens it.
    r.reopen(FakeKeymanager::Mode::Working);
    CHECK(r.secrets->protection() == Protection::Keymanager3);
    CHECK(r.secrets->get("server.token", &out) == SecretStatus::Ok);
    CHECK_EQ(out, std::string("s3cr3t-token-value"));
}

TEST("secrets: without keymanager3 the plain envelope is used and reported") {
    for (auto mode : {FakeKeymanager::Mode::Absent, FakeKeymanager::Mode::Denied}) {
        Rig r(mode);
        CHECK(r.secrets->protection() == Protection::Plain);
        CHECK(!r.secrets->last_error().empty());
        CHECK(r.secrets->put("pin", "1234") == SecretStatus::Ok);
        CHECK(r.raw("pin").find("plain-v1") != std::string::npos);
        std::string out;
        CHECK(r.secrets->get("pin", &out) == SecretStatus::Ok);
        CHECK_EQ(out, std::string("1234"));
    }
    // No transport at all behaves the same.
    char tmpl[] = "/tmp/wnk-secret-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    auto kv = open_file_store(dir);
    SecretStore s(*kv, nullptr, "x");
    CHECK(s.protection() == Protection::Plain);
    std::system(("rm -rf '" + dir + "'").c_str());
}

TEST("secrets: no silent downgrade when keymanager3 goes away") {
    Rig r;
    REQUIRE(r.secrets->put("token", "abc") == SecretStatus::Ok);
    r.reopen(FakeKeymanager::Mode::Denied);
    std::string out;
    CHECK(r.secrets->get("token", &out) == SecretStatus::Locked);
    CHECK(r.secrets->put("token", "replacement") == SecretStatus::Locked);  // would have stored it in the clear
    CHECK(r.raw("token").find("keymanager3-aes256-gcm-v1") != std::string::npos);  // data intact
    r.reopen(FakeKeymanager::Mode::Working);
    CHECK(r.secrets->get("token", &out) == SecretStatus::Ok);
    CHECK_EQ(out, std::string("abc"));
}

TEST("secrets: tampered ciphertext is refused and the operation aborted") {
    Rig r;
    REQUIRE(r.secrets->put("token", "abc") == SecretStatus::Ok);
    json env = json::parse(r.raw("token"));
    env["data"] = std::string("x") + env["data"].get<std::string>();
    r.kv->put("secret.token", env.dump());
    std::string out;
    CHECK(r.secrets->get("token", &out) == SecretStatus::Locked);
    CHECK(r.km->aborts >= 1);
    CHECK(r.secrets->last_error().find("finish(decrypt)") != std::string::npos);
}

TEST("secrets: names are validated; remove and not-found behave") {
    Rig r;
    CHECK(r.secrets->put("Bad Name", "x") == SecretStatus::InvalidName);
    CHECK(r.secrets->put(std::string(57, 'a'), "x") == SecretStatus::InvalidName);
    std::string out;
    CHECK(r.secrets->get("missing", &out) == SecretStatus::NotFound);
    REQUIRE(r.secrets->put("token", "abc") == SecretStatus::Ok);
    CHECK(r.secrets->remove("token") == SecretStatus::Ok);
    CHECK(r.secrets->get("token", &out) == SecretStatus::NotFound);
    CHECK(r.secrets->remove("token") == SecretStatus::NotFound);
}
