// SPDX-License-Identifier: GPL-3.0-or-later
//
// Image loading for TV UIs: decode, downscale, cache and fetch, without touching the GPU.
//
//   request ─► memory cache ─► disk cache ─► HTTP / file ─► decode + fit ─► memory cache ─► main thread
//
// The service produces CPU-side RGBA images (ImageRGBA). Turning them into textures is the renderer's
// job (see wnk/image/textures.hpp), so texture ownership never leaves the render thread.
//
// Threading: ImageService::request/cancel/prefetch are called on the main thread; callbacks run on
// the main thread (through the MainDispatcher). Fetching and decoding run on the WorkerPool. The
// dispatcher must outlive the pool (shut the pool down first), as with every async::WorkerPool user.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/net/http.hpp"

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wnk::image {

struct ImageRGBA {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;  // width*height*4, straight alpha, top row first
    size_t bytes() const { return pixels.size(); }
};

enum class ImageError { None, Network, HttpStatus, NotFound, Decode, TooLarge, Cancelled, Unsupported };
const char *error_name(ImageError e);

struct DecodeLimits {
    size_t max_encoded_bytes = 16u << 20;
    int max_dimension = 8192;            // either side, before scaling
    uint64_t max_pixels = 8192ull * 8192ull;
};

struct DecodeResult {
    ImageError error = ImageError::None;
    std::string detail;
    std::shared_ptr<ImageRGBA> image;
};

// Decode an image to RGBA. The format is taken from the bytes, not from a file name or Content-Type:
//   PNG, JPEG, BMP, GIF (first frame), WebP (lossy, lossless, alpha; first frame of an animation),
//   SVG (nanosvg: shapes, paths, gradients, transforms; no text, filters, CSS or embedded images).
// Raster images are downscaled (never enlarged) to fit inside max_w/max_h when > 0, keeping the
// aspect ratio. An SVG is drawn at the size that fits max_w/max_h, enlarged if need be, so a vector icon
// stays sharp. Dimensions are checked against `limits` before any pixel memory is allocated.
DecodeResult decode_image(const std::string &bytes, int max_w = 0, int max_h = 0, const DecodeLimits &limits = {});

// "png", "jpeg", "gif", "bmp", "webp", "svg", or "" when the bytes are none of these.
const char *sniff_format(const std::string &bytes);

// Area-averaging downscale to exactly dst_w x dst_h (each <= source size).
ImageRGBA downscale(const ImageRGBA &src, int dst_w, int dst_h);

// Size that fits (w, h) inside (max_w, max_h) keeping aspect ratio; never larger than (w, h); 0 = no limit.
void fit_size(int w, int h, int max_w, int max_h, int *out_w, int *out_h);

// ---- caches ----------------------------------------------------------------------------------------

// Decoded images, least-recently-used eviction by pixel bytes. Thread-safe.
class MemoryCache {
 public:
    explicit MemoryCache(size_t max_bytes) : max_bytes_(max_bytes) {}
    std::shared_ptr<const ImageRGBA> get(const std::string &key);
    void put(const std::string &key, std::shared_ptr<const ImageRGBA> image);
    void trim(size_t target_bytes);
    size_t bytes() const;
    size_t entries() const;

 private:
    using Order = std::list<std::string>;
    struct Slot {
        std::shared_ptr<const ImageRGBA> image;
        Order::iterator pos;
    };
    void evict_locked(size_t target);
    mutable std::mutex mutex_;
    size_t max_bytes_;
    size_t bytes_ = 0;
    Order order_;  // front = most recent
    std::unordered_map<std::string, Slot> map_;
};

// Encoded image bytes on disk, keyed by URL, bounded in total size, least-recently-used eviction.
// Disposable: corrupt or unreadable entries are deleted and treated as misses. Thread-safe.
class DiskCache {
 public:
    // `directory` is created if missing. Existing entries are indexed (oldest first by mtime).
    DiskCache(std::string directory, uint64_t max_bytes);
    bool usable() const { return usable_; }
    std::optional<std::string> get(const std::string &url);
    bool put(const std::string &url, const std::string &bytes);
    uint64_t bytes() const;
    size_t entries() const;
    void clear();

 private:
    std::string path_for(const std::string &url) const;
    void evict_locked(uint64_t target);
    void touch_locked(const std::string &file);
    mutable std::mutex mutex_;
    std::string dir_;
    uint64_t max_bytes_;
    bool usable_ = false;
    uint64_t bytes_ = 0;
    std::list<std::pair<std::string, uint64_t>> lru_;  // (file name, size); front = most recent
    std::map<std::string, std::list<std::pair<std::string, uint64_t>>::iterator> index_;
};

// ---- service ---------------------------------------------------------------------------------------

struct ImageRequest {
    std::string url;     // https://, http:// or file:///absolute/path
    int max_width = 0;   // 0 = original size
    int max_height = 0;
};

struct ImageResult {
    ImageError error = ImageError::None;
    std::shared_ptr<const ImageRGBA> image;
    std::string detail;
    std::string key;     // cache key (url + size); stable identity for texture caches
    bool from_memory = false;
};

using ImageTicket = uint64_t;  // 0 = invalid

struct ImageServiceConfig {
    size_t memory_bytes = 96u << 20;
    std::string disk_directory;          // empty = no disk cache
    uint64_t disk_bytes = 128u << 20;
    size_t max_download_bytes = 16u << 20;
    int timeout_ms = 15000;
    DecodeLimits limits;
};

struct ImageStats {
    uint64_t requests = 0, memory_hits = 0, disk_hits = 0, downloads = 0, coalesced = 0, failures = 0;
    size_t in_flight = 0, memory_entries = 0, memory_bytes = 0, disk_entries = 0;
    uint64_t disk_bytes = 0;
};

class ImageService {
 public:
    using Callback = std::function<void(ImageResult)>;

    ImageService(std::shared_ptr<net::HttpTransport> transport, async::WorkerPool &pool, async::MainDispatcher &main,
                 ImageServiceConfig config = {});
    ~ImageService();

    // Main thread. The callback runs once, on the main thread, unless the ticket is cancelled or the
    // owner is gone. A memory hit is still delivered through the dispatcher (never re-entrantly).
    ImageTicket request(const ImageRequest &req, async::Lifetime::Weak owner, Callback on_done);
    void cancel(ImageTicket ticket);
    // Fill caches without a callback (e.g. the next row of posters).
    void prefetch(const ImageRequest &req);

    static std::string cache_key(const ImageRequest &req);
    ImageStats stats() const;
    MemoryCache &memory();

    struct Core;  // caches, transport and stats, shared with worker tasks so they outlive the service

 private:
    struct Waiter {
        ImageTicket ticket;
        async::Lifetime::Weak owner;
        Callback callback;
    };
    struct Flight {
        uint64_t id = 0;
        std::vector<Waiter> waiters;
        async::CancellationSource cancel;
    };
    ImageTicket start(const ImageRequest &req, async::Lifetime::Weak owner, Callback cb, bool with_callback);
    void finish(const std::string &key, uint64_t flight_id, ImageResult result);  // main thread

    std::shared_ptr<Core> core_;
    async::WorkerPool &pool_;
    async::MainDispatcher &main_;
    std::map<std::string, Flight> flights_;       // main thread only
    std::map<ImageTicket, std::string> tickets_;  // main thread only; live, uncancelled tickets
    ImageTicket next_ticket_ = 1;
    uint64_t next_flight_ = 1;
    async::Lifetime lifetime_;
};

}  // namespace wnk::image
