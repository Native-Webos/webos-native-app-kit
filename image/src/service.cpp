// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/image/image.hpp"

#include "wnk/log.hpp"

#include <algorithm>

#include <fcntl.h>
#include <unistd.h>

namespace wnk::image {

struct ImageService::Core {
    std::shared_ptr<net::HttpTransport> transport;
    ImageServiceConfig config;
    MemoryCache memory;
    std::unique_ptr<DiskCache> disk;
    std::mutex stats_mutex;
    ImageStats stats;

    Core(std::shared_ptr<net::HttpTransport> t, ImageServiceConfig c)
        : transport(std::move(t)), config(std::move(c)), memory(config.memory_bytes) {
        if (!config.disk_directory.empty()) {
            disk = std::make_unique<DiskCache>(config.disk_directory, config.disk_bytes);
            if (!disk->usable()) {
                WNK_LOGW("image", "disk cache unavailable at %s", config.disk_directory.c_str());
                disk.reset();
            }
        }
    }

    template <typename F>
    void bump(F f) {
        std::lock_guard<std::mutex> lock(stats_mutex);
        f(stats);
    }

    bool read_local(const std::string &path, std::string *out, std::string *why) const {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            *why = "cannot open " + path;
            return false;
        }
        char buf[65536];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof buf)) > 0) {
            out->append(buf, static_cast<size_t>(n));
            if (out->size() > config.max_download_bytes) {
                ::close(fd);
                *why = "file exceeds limit";
                return false;
            }
        }
        ::close(fd);
        return n == 0;
    }

    // Worker thread: disk cache, then file or HTTP, then decode; fills both caches on success.
    ImageResult fetch(const ImageRequest &req, const std::string &key, const async::CancellationToken &t) {
        ImageResult r;
        r.key = key;
        std::string bytes;
        bool from_disk = false;
        const bool local = req.url.rfind("file://", 0) == 0;
        if (!local && disk) {
            if (auto hit = disk->get(req.url)) {
                bytes = std::move(*hit);
                from_disk = true;
                bump([](ImageStats &s) { ++s.disk_hits; });
            }
        }
        if (t.cancelled()) {
            r.error = ImageError::Cancelled;
            return r;
        }
        if (!from_disk) {
            if (local) {
                if (!read_local(req.url.substr(7), &bytes, &r.detail)) {
                    r.error = r.detail.find("limit") != std::string::npos ? ImageError::TooLarge : ImageError::NotFound;
                    return r;
                }
            } else {
                if (!transport) {
                    r.error = ImageError::Unsupported;
                    r.detail = "no HTTP transport";
                    return r;
                }
                net::HttpRequest http;
                http.url = req.url;
                http.timeout_ms = config.timeout_ms;
                http.max_response_bytes = config.max_download_bytes;
                http.headers = {{"Accept", "image/png, image/jpeg"}};
                bump([](ImageStats &s) { ++s.downloads; });
                net::HttpResponse resp = transport->perform(http, t);
                if (resp.error == net::NetError::Cancelled) {
                    r.error = ImageError::Cancelled;
                    return r;
                }
                if (resp.error == net::NetError::TooLarge) {
                    r.error = ImageError::TooLarge;
                    r.detail = "download exceeds limit";
                    return r;
                }
                if (resp.error != net::NetError::None) {
                    r.error = ImageError::Network;
                    r.detail = std::string(net::error_name(resp.error)) + ": " + resp.detail;
                    return r;
                }
                if (resp.status == 404 || resp.status == 410) {
                    r.error = ImageError::NotFound;
                    r.detail = "HTTP " + std::to_string(resp.status);
                    return r;
                }
                if (resp.status < 200 || resp.status >= 300) {
                    r.error = ImageError::HttpStatus;
                    r.detail = "HTTP " + std::to_string(resp.status);
                    return r;
                }
                bytes = std::move(resp.body);
            }
        }
        if (t.cancelled()) {
            r.error = ImageError::Cancelled;
            return r;
        }
        DecodeResult d = decode_image(bytes, req.max_width, req.max_height, config.limits);
        if (d.error != ImageError::None) {
            r.error = d.error;
            r.detail = d.detail;
            return r;
        }
        // Only bytes that decode are worth keeping on disk.
        if (!local && !from_disk && disk) disk->put(req.url, bytes);
        memory.put(key, d.image);
        r.image = std::move(d.image);
        return r;
    }
};

ImageService::ImageService(std::shared_ptr<net::HttpTransport> transport, async::WorkerPool &pool,
                           async::MainDispatcher &main, ImageServiceConfig config)
    : core_(std::make_shared<Core>(std::move(transport), std::move(config))), pool_(pool), main_(main) {}

ImageService::~ImageService() {
    // Workers keep `core_` alive; results posted after this point are ignored via lifetime_.
    for (auto &[key, f] : flights_) f.cancel.cancel();
}

MemoryCache &ImageService::memory() { return core_->memory; }

std::string ImageService::cache_key(const ImageRequest &req) {
    return req.url + "#" + std::to_string(req.max_width) + "x" + std::to_string(req.max_height);
}

ImageTicket ImageService::request(const ImageRequest &req, async::Lifetime::Weak owner, Callback on_done) {
    return start(req, std::move(owner), std::move(on_done), true);
}

void ImageService::prefetch(const ImageRequest &req) { start(req, {}, nullptr, false); }

ImageTicket ImageService::start(const ImageRequest &req, async::Lifetime::Weak owner, Callback cb,
                                bool with_callback) {
    const std::string key = cache_key(req);
    core_->bump([](ImageStats &s) { ++s.requests; });
    const ImageTicket ticket = with_callback ? next_ticket_++ : 0;
    if (ticket != 0) tickets_[ticket] = key;

    if (auto hit = core_->memory.get(key)) {
        core_->bump([](ImageStats &s) { ++s.memory_hits; });
        if (ticket != 0) {
            ImageResult r;
            r.image = std::move(hit);
            r.key = key;
            r.from_memory = true;
            // Delivered through the dispatcher so callers never see a callback inside request().
            main_.post([this, alive = lifetime_.weak(), ticket, owner, cb = std::move(cb), r]() {
                if (!alive.alive() || tickets_.erase(ticket) == 0 || !owner.alive()) return;
                cb(r);
            });
        }
        return ticket;
    }

    auto it = flights_.find(key);
    if (it != flights_.end()) {
        core_->bump([](ImageStats &s) { ++s.coalesced; });
        if (ticket != 0) it->second.waiters.push_back({ticket, std::move(owner), std::move(cb)});
        return ticket;
    }

    Flight &f = flights_[key];
    f.id = next_flight_++;
    if (ticket != 0) f.waiters.push_back({ticket, std::move(owner), std::move(cb)});
    core_->bump([](ImageStats &s) { ++s.in_flight; });
    const uint64_t flight_id = f.id;
    const bool queued = pool_.submit(
        [core = core_, req, key, flight_id, &main = main_, svc = this, alive = lifetime_.weak()](
            const async::CancellationToken &t) {
            ImageResult r = core->fetch(req, key, t);
            if (r.error != ImageError::None && r.error != ImageError::Cancelled) {
                // Without the query: image URLs of media servers often carry an access token there.
                const std::string url = req.url.substr(0, req.url.find('?'));
                WNK_LOGW("image", "%s failed: %s %s", url.c_str(), error_name(r.error), r.detail.c_str());
            }
            main.post([svc, alive, key, flight_id, r = std::move(r)]() mutable {
                if (alive.alive()) svc->finish(key, flight_id, std::move(r));
            });
        },
        f.cancel.token());
    if (!queued) {
        ImageResult r;
        r.error = ImageError::Cancelled;
        r.key = key;
        r.detail = "worker pool is shutting down";
        main_.post([svc = this, alive = lifetime_.weak(), key, flight_id, r]() mutable {
            if (alive.alive()) svc->finish(key, flight_id, std::move(r));
        });
    }
    return ticket;
}

void ImageService::finish(const std::string &key, uint64_t flight_id, ImageResult result) {
    auto it = flights_.find(key);
    if (it == flights_.end() || it->second.id != flight_id) return;  // cancelled and possibly restarted
    Flight flight = std::move(it->second);
    flights_.erase(it);
    core_->bump([&](ImageStats &s) {
        --s.in_flight;
        if (result.error != ImageError::None && result.error != ImageError::Cancelled) ++s.failures;
    });
    for (auto &w : flight.waiters) {
        if (tickets_.erase(w.ticket) == 0 || !w.owner.alive()) continue;
        w.callback(result);
    }
}

void ImageService::cancel(ImageTicket ticket) {
    auto t = tickets_.find(ticket);
    if (t == tickets_.end()) return;
    const std::string key = t->second;
    tickets_.erase(t);
    auto it = flights_.find(key);
    if (it == flights_.end()) return;  // a memory hit still waiting in the dispatcher: now suppressed
    auto &waiters = it->second.waiters;
    waiters.erase(std::remove_if(waiters.begin(), waiters.end(), [&](const Waiter &w) { return w.ticket == ticket; }),
                  waiters.end());
    if (waiters.empty()) {
        // Nobody wants it any more: stop the download and forget the flight, so a new request starts
        // fresh instead of joining a cancelled one.
        it->second.cancel.cancel();
        flights_.erase(it);
        core_->bump([](ImageStats &s) { --s.in_flight; });
    }
}

ImageStats ImageService::stats() const {
    ImageStats s;
    {
        std::lock_guard<std::mutex> lock(core_->stats_mutex);
        s = core_->stats;
    }
    s.memory_entries = core_->memory.entries();
    s.memory_bytes = core_->memory.bytes();
    if (core_->disk) {
        s.disk_entries = core_->disk->entries();
        s.disk_bytes = core_->disk->bytes();
    }
    return s;
}

}  // namespace wnk::image
