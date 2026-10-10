// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/image/image.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

using namespace wnk;
using namespace wnk::image;
using namespace std::chrono_literals;

namespace {

void append(void *ctx, void *data, int size) {
    static_cast<std::string *>(ctx)->append(static_cast<const char *>(data), static_cast<size_t>(size));
}

// w x h image: left half red, right half blue, fully opaque.
std::vector<uint8_t> halves(int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t *p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = x < w / 2 ? 255 : 0;
            p[2] = x < w / 2 ? 0 : 255;
            p[3] = 255;
        }
    }
    return px;
}

std::string png(int w, int h) {
    std::string out;
    auto px = halves(w, h);
    stbi_write_png_to_func(append, &out, w, h, 4, px.data(), w * 4);
    return out;
}

std::string jpeg(int w, int h) {
    std::string out;
    auto px = halves(w, h);
    stbi_write_jpg_to_func(append, &out, w, h, 4, px.data(), 95);
    return out;
}

std::string temp_dir() {
    char tmpl[] = "/tmp/wnk-img-XXXXXX";
    const char *d = mkdtemp(tmpl);
    return d != nullptr ? std::string(d) : std::string();
}

void remove_tree(const std::string &dir) {
    if (dir.rfind("/tmp/wnk-img-", 0) == 0) (void)std::system(("rm -rf '" + dir + "'").c_str());
}

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit = 5000ms) {
    const auto end = async::Clock::now() + limit;
    while (!pred()) {
        if (async::Clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// Serves bytes per URL, counts requests, can be slowed down.
struct FakeHttp final : net::HttpTransport {
    std::mutex m;
    std::map<std::string, std::pair<long, std::string>> routes;
    std::atomic<int> calls{0};
    std::chrono::milliseconds delay{0};
    net::HttpResponse perform(const net::HttpRequest &req, const async::CancellationToken &t) override {
        ++calls;
        const auto end = async::Clock::now() + delay;
        while (async::Clock::now() < end) {
            if (t.cancelled()) {
                net::HttpResponse r;
                r.error = net::NetError::Cancelled;
                return r;
            }
            std::this_thread::sleep_for(1ms);
        }
        std::lock_guard<std::mutex> lock(m);
        net::HttpResponse r;
        auto it = routes.find(req.url);
        r.status = it == routes.end() ? 404 : it->second.first;
        if (it != routes.end()) r.body = it->second.second;
        return r;
    }
    std::string describe() const override { return "fake"; }
};

struct Env {
    std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
    async::MainDispatcher main;
    async::WorkerPool pool{2};
    std::unique_ptr<ImageService> svc;
    explicit Env(ImageServiceConfig cfg = {}) { svc = std::make_unique<ImageService>(http, pool, main, cfg); }
    ~Env() {
        svc.reset();
        main.close();
        pool.shutdown(2000ms);
    }
    // Pump the main queue until `pred` or timeout.
    template <typename Pred>
    bool pump(Pred pred) {
        return wait_until([&] {
            main.drain();
            return pred();
        });
    }
};

}  // namespace

TEST("fit_size keeps aspect and never upscales") {
    int w, h;
    fit_size(4000, 6000, 400, 400, &w, &h);
    CHECK_EQ(w, 267);
    CHECK_EQ(h, 400);
    fit_size(100, 50, 400, 400, &w, &h);
    CHECK_EQ(w, 100);
    CHECK_EQ(h, 50);
    fit_size(1000, 10, 100, 0, &w, &h);
    CHECK_EQ(w, 100);
    CHECK_EQ(h, 1);
}

TEST("PNG and JPEG decode to RGBA") {
    DecodeResult p = decode_image(png(64, 32));
    REQUIRE(p.error == ImageError::None);
    CHECK_EQ(p.image->width, 64);
    CHECK_EQ(p.image->height, 32);
    CHECK_EQ(p.image->pixels[0], 255);  // red at the left
    CHECK_EQ(p.image->pixels[(63) * 4 + 2], 255);  // blue at the right
    DecodeResult j = decode_image(jpeg(64, 32));
    REQUIRE(j.error == ImageError::None);
    CHECK(j.image->pixels[0] > 200);
    CHECK_EQ(j.image->pixels[3], 255);
}

TEST("decode downscales to fit and averages colours") {
    DecodeResult r = decode_image(png(400, 200), 100, 100);
    REQUIRE(r.error == ImageError::None);
    CHECK_EQ(r.image->width, 100);
    CHECK_EQ(r.image->height, 50);
    const uint8_t *left = &r.image->pixels[(25 * 100 + 10) * 4];
    const uint8_t *right = &r.image->pixels[(25 * 100 + 90) * 4];
    CHECK(left[0] == 255 && left[2] == 0);
    CHECK(right[0] == 0 && right[2] == 255);
    ImageRGBA two;  // red | blue -> one purple pixel
    two.width = 2;
    two.height = 1;
    two.pixels = {255, 0, 0, 255, 0, 0, 255, 255};
    ImageRGBA one = downscale(two, 1, 1);
    CHECK(std::abs(one.pixels[0] - 128) <= 1 && std::abs(one.pixels[2] - 128) <= 1);
}

TEST("transparent pixels do not darken downscaled edges") {
    ImageRGBA src;
    src.width = 2;
    src.height = 1;
    src.pixels = {255, 255, 255, 255, 0, 0, 0, 0};  // white | fully transparent black
    ImageRGBA one = downscale(src, 1, 1);
    CHECK_EQ(one.pixels[0], 255);
    CHECK(std::abs(one.pixels[3] - 128) <= 1);
}

TEST("garbage, empty and oversized inputs are rejected before allocation") {
    CHECK(decode_image("not an image").error == ImageError::Decode);
    CHECK(decode_image("").error == ImageError::Decode);
    std::string huge = png(16, 16);
    // Patch the IHDR width/height to 100000 x 100000 (bytes 16..23, big-endian).
    const uint32_t big = 100000;
    for (int i = 0; i < 4; ++i) {
        huge[16 + i] = static_cast<char>((big >> (24 - 8 * i)) & 0xFF);
        huge[20 + i] = static_cast<char>((big >> (24 - 8 * i)) & 0xFF);
    }
    DecodeResult r = decode_image(huge);
    CHECK(r.error == ImageError::TooLarge);
    // Same for a JPEG whose frame header claims 60000 x 60000.
    std::string jbig = jpeg(16, 16);
    for (size_t i = 2; i + 9 < jbig.size(); ++i) {
        if (static_cast<unsigned char>(jbig[i]) == 0xFF && static_cast<unsigned char>(jbig[i + 1]) == 0xC0) {
            jbig[i + 5] = jbig[i + 7] = static_cast<char>(0xEA);
            jbig[i + 6] = jbig[i + 8] = static_cast<char>(0x60);
            break;
        }
    }
    CHECK(decode_image(jbig).error == ImageError::TooLarge);
    DecodeLimits tight;
    tight.max_dimension = 8;
    CHECK(decode_image(png(16, 16), 0, 0, tight).error == ImageError::TooLarge);
    DecodeLimits small;
    small.max_encoded_bytes = 10;
    CHECK(decode_image(png(16, 16), 0, 0, small).error == ImageError::TooLarge);
}

TEST("memory cache evicts least recently used by bytes") {
    MemoryCache c(3 * 400);  // three 10x10 RGBA images
    auto img = [] {
        auto i = std::make_shared<ImageRGBA>();
        i->width = i->height = 10;
        i->pixels.resize(400);
        return i;
    };
    c.put("a", img());
    c.put("b", img());
    c.put("c", img());
    CHECK(c.get("a") != nullptr);  // a becomes most recent
    c.put("d", img());             // evicts b
    CHECK(c.get("b") == nullptr);
    CHECK(c.get("a") != nullptr);
    CHECK_EQ(c.entries(), 3u);
    CHECK_EQ(c.bytes(), 1200u);
    c.trim(400);
    CHECK_EQ(c.entries(), 1u);
    auto big = std::make_shared<ImageRGBA>();
    big->pixels.resize(5000);
    c.put("big", big);  // larger than the whole cache: ignored
    CHECK(c.get("big") == nullptr);
}

TEST("disk cache persists, evicts by size and drops corrupt entries") {
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    {
        DiskCache d(dir + "/cache", 3000);
        REQUIRE(d.usable());
        CHECK(d.put("https://x/1", std::string(1000, '1')));
        CHECK(d.put("https://x/2", std::string(1000, '2')));
        CHECK(d.get("https://x/1").has_value());  // 1 is now most recent
        CHECK(d.put("https://x/3", std::string(1000, '3')));  // over budget: evicts 2
        CHECK(!d.get("https://x/2").has_value());
        CHECK(!d.put("https://x/huge", std::string(5000, 'h')));
    }
    DiskCache d(dir + "/cache", 3000);  // reopen: index rebuilt from files
    auto one = d.get("https://x/1");
    REQUIRE(one.has_value());
    CHECK_EQ(*one, std::string(1000, '1'));
    CHECK_EQ(d.entries(), 2u);
    // A stray, unindexed file in the directory is ignored and does not confuse lookups.
    std::ofstream(dir + "/cache/0000000000000000.img") << "garbage";
    CHECK(!d.get("https://x/never").has_value());
    d.clear();
    CHECK_EQ(d.entries(), 0u);
    CHECK_EQ(d.bytes(), 0u);
    remove_tree(dir);
}

TEST("disk cache treats a truncated file as a miss") {
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    DiskCache d(dir, 100000);
    REQUIRE(d.put("https://x/t", std::string(500, 't')));
    // Truncate every .img file in the directory.
    (void)std::system(("for f in '" + dir + "'/*.img; do : > \"$f\"; done").c_str());
    CHECK(!d.get("https://x/t").has_value());
    CHECK_EQ(d.entries(), 0u);
    remove_tree(dir);
}

TEST("service downloads, decodes, caches and coalesces duplicates") {
    Env env;
    env.http->routes["https://img/a.png"] = {200, png(200, 100)};
    env.http->delay = 50ms;
    async::Lifetime owner;
    int done = 0;
    ImageResult first, second;
    env.svc->request({"https://img/a.png", 50, 50}, owner.weak(), [&](ImageResult r) { first = r; ++done; });
    env.svc->request({"https://img/a.png", 50, 50}, owner.weak(), [&](ImageResult r) { second = r; ++done; });
    CHECK(env.pump([&] { return done == 2; }));
    CHECK_EQ(env.http->calls.load(), 1);
    REQUIRE(first.image != nullptr);
    CHECK_EQ(first.image->width, 50);
    CHECK_EQ(first.image->height, 25);
    CHECK(first.image == second.image);
    CHECK_EQ(env.svc->stats().coalesced, 1u);

    ImageResult third;
    env.svc->request({"https://img/a.png", 50, 50}, owner.weak(), [&](ImageResult r) { third = r; ++done; });
    CHECK_EQ(done, 2);  // never delivered inside request()
    CHECK(env.pump([&] { return done == 3; }));
    CHECK(third.from_memory);
    CHECK_EQ(env.http->calls.load(), 1);
}

TEST("cancelling every waiter stops the download; cancelling one does not") {
    Env env;
    env.http->routes["https://img/slow.png"] = {200, png(10, 10)};
    env.http->delay = 1000ms;
    async::Lifetime owner;
    bool a_called = false, b_called = false;
    ImageTicket a = env.svc->request({"https://img/slow.png"}, owner.weak(), [&](ImageResult) { a_called = true; });
    ImageTicket b = env.svc->request({"https://img/slow.png"}, owner.weak(), [&](ImageResult) { b_called = true; });
    CHECK(wait_until([&] { return env.http->calls.load() == 1; }));
    env.svc->cancel(a);
    CHECK_EQ(env.svc->stats().in_flight, 1u);
    env.svc->cancel(b);
    CHECK_EQ(env.svc->stats().in_flight, 0u);
    CHECK(wait_until([&] { return env.pool.stats().completed == 1; }, 800ms));  // stopped early, not after 1 s
    env.main.drain();
    CHECK(!a_called && !b_called);
    // A new request starts a fresh download instead of joining the cancelled one.
    env.http->delay = 0ms;
    bool c_ok = false;
    env.svc->request({"https://img/slow.png"}, owner.weak(), [&](ImageResult r) { c_ok = r.image != nullptr; });
    CHECK(env.pump([&] { return c_ok; }));
    CHECK_EQ(env.http->calls.load(), 2);
}

TEST("results for destroyed owners are dropped") {
    Env env;
    env.http->routes["https://img/b.png"] = {200, png(8, 8)};
    env.http->delay = 30ms;
    bool called = false;
    {
        async::Lifetime owner;
        env.svc->request({"https://img/b.png"}, owner.weak(), [&](ImageResult) { called = true; });
    }
    CHECK(wait_until([&] { return env.pool.stats().completed == 1; }));
    env.main.drain();
    CHECK(!called);
    CHECK(env.svc->memory().get(ImageService::cache_key({"https://img/b.png"})) != nullptr);  // still cached
}

TEST("errors are reported and failed downloads are not cached on disk") {
    const std::string dir = temp_dir();
    ImageServiceConfig cfg;
    cfg.disk_directory = dir + "/images";
    Env env(cfg);
    env.http->routes["https://img/bad.png"] = {200, "this is not a png"};
    env.http->routes["https://img/err.png"] = {500, "oops"};
    async::Lifetime owner;
    std::map<std::string, ImageError> errors;
    for (const char *u : {"https://img/missing.png", "https://img/bad.png", "https://img/err.png"}) {
        env.svc->request({u}, owner.weak(), [&, u](ImageResult r) { errors[u] = r.error; });
    }
    CHECK(env.pump([&] { return errors.size() == 3; }));
    CHECK(errors["https://img/missing.png"] == ImageError::NotFound);
    CHECK(errors["https://img/bad.png"] == ImageError::Decode);
    CHECK(errors["https://img/err.png"] == ImageError::HttpStatus);
    CHECK_EQ(env.svc->stats().failures, 3u);
    CHECK_EQ(env.svc->stats().disk_entries, 0u);
    remove_tree(dir);
}

TEST("disk cache serves a restarted service without the network") {
    const std::string dir = temp_dir();
    ImageServiceConfig cfg;
    cfg.disk_directory = dir + "/images";
    {
        Env env(cfg);
        env.http->routes["https://img/c.jpg"] = {200, jpeg(32, 32)};
        async::Lifetime owner;
        bool ok = false;
        env.svc->request({"https://img/c.jpg"}, owner.weak(), [&](ImageResult r) { ok = r.image != nullptr; });
        CHECK(env.pump([&] { return ok; }));
        CHECK_EQ(env.svc->stats().disk_entries, 1u);
    }
    Env env(cfg);  // new service, empty memory, same disk; routes empty: network would 404
    async::Lifetime owner;
    ImageResult r;
    bool done = false;
    env.svc->request({"https://img/c.jpg", 16, 16}, owner.weak(), [&](ImageResult x) { r = x; done = true; });
    CHECK(env.pump([&] { return done; }));
    REQUIRE(r.image != nullptr);
    CHECK_EQ(r.image->width, 16);
    CHECK_EQ(env.http->calls.load(), 0);
    CHECK_EQ(env.svc->stats().disk_hits, 1u);
    remove_tree(dir);
}

TEST("file URLs load bundled images") {
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    std::ofstream(dir + "/poster.png", std::ios::binary) << png(20, 30);
    Env env;
    async::Lifetime owner;
    ImageResult r;
    bool done = false;
    env.svc->request({"file://" + dir + "/poster.png"}, owner.weak(), [&](ImageResult x) { r = x; done = true; });
    CHECK(env.pump([&] { return done; }));
    REQUIRE(r.image != nullptr);
    CHECK_EQ(r.image->height, 30);
    bool missing_done = false;
    ImageError missing = ImageError::None;
    env.svc->request({"file://" + dir + "/nope.png"}, owner.weak(), [&](ImageResult x) {
        missing = x.error;
        missing_done = true;
    });
    CHECK(env.pump([&] { return missing_done; }));
    CHECK(missing == ImageError::NotFound);
    CHECK_EQ(env.http->calls.load(), 0);
    remove_tree(dir);
}

TEST("prefetch warms the cache without a callback") {
    Env env;
    env.http->routes["https://img/p.png"] = {200, png(10, 10)};
    env.svc->prefetch({"https://img/p.png"});
    CHECK(wait_until([&] {
        env.main.drain();
        return env.svc->memory().get(ImageService::cache_key({"https://img/p.png"})) != nullptr;
    }));
    CHECK_EQ(env.svc->stats().in_flight, 0u);
}
