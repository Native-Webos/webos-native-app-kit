// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/image/image.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wnk::image {

// ---- MemoryCache -----------------------------------------------------------------------------------

std::shared_ptr<const ImageRGBA> MemoryCache::get(const std::string &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    order_.splice(order_.begin(), order_, it->second.pos);
    return it->second.image;
}

void MemoryCache::put(const std::string &key, std::shared_ptr<const ImageRGBA> image) {
    if (!image) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (image->bytes() > max_bytes_) return;  // would evict everything and still not fit
    auto it = map_.find(key);
    if (it != map_.end()) {
        bytes_ -= it->second.image->bytes();
        order_.erase(it->second.pos);
        map_.erase(it);
    }
    order_.push_front(key);
    bytes_ += image->bytes();
    map_[key] = Slot{std::move(image), order_.begin()};
    evict_locked(max_bytes_);
}

void MemoryCache::trim(size_t target_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    evict_locked(target_bytes);
}

void MemoryCache::evict_locked(size_t target) {
    while (bytes_ > target && !order_.empty()) {
        auto it = map_.find(order_.back());
        bytes_ -= it->second.image->bytes();
        map_.erase(it);
        order_.pop_back();
    }
}

size_t MemoryCache::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

size_t MemoryCache::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

// ---- DiskCache -------------------------------------------------------------------------------------
//
// One file per URL: "<fnv1a64-hex>.img" containing "WNKIMG1 <url-length>\n<url>\n<bytes>". The URL is
// stored and compared on read, so a hash collision is a miss rather than a wrong image.

namespace {

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

}  // namespace

DiskCache::DiskCache(std::string directory, uint64_t max_bytes) : dir_(std::move(directory)), max_bytes_(max_bytes) {
    if (dir_.empty() || !make_dirs(dir_)) return;
    usable_ = true;
    std::vector<std::pair<time_t, std::pair<std::string, uint64_t>>> found;
    if (DIR *d = ::opendir(dir_.c_str())) {
        while (dirent *e = ::readdir(d)) {
            const std::string name = e->d_name;
            const std::string full = dir_ + "/" + name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
                ::unlink(full.c_str());  // interrupted write
                continue;
            }
            if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".img") != 0) continue;
            struct stat st{};
            if (::lstat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            found.push_back({st.st_mtime, {name, static_cast<uint64_t>(st.st_size)}});
        }
        ::closedir(d);
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &f : found) {  // oldest first, so the newest ends up at the front
        lru_.push_front(f.second);
        index_[f.second.first] = lru_.begin();
        bytes_ += f.second.second;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    evict_locked(max_bytes_);
}

std::string DiskCache::path_for(const std::string &url) const {
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.img", static_cast<unsigned long long>(fnv1a(url)));
    return name;
}

void DiskCache::touch_locked(const std::string &file) {
    auto it = index_.find(file);
    if (it == index_.end()) return;
    lru_.splice(lru_.begin(), lru_, it->second);
    ::utimensat(AT_FDCWD, (dir_ + "/" + file).c_str(), nullptr, 0);  // keep order across restarts
}

std::optional<std::string> DiskCache::get(const std::string &url) {
    if (!usable_) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string file = path_for(url);
    auto it = index_.find(file);
    if (it == index_.end()) return std::nullopt;
    std::string data;
    const std::string full = dir_ + "/" + file;
    const std::string header_prefix = "WNKIMG1 " + std::to_string(url.size()) + "\n";
    const bool ok = read_file(full, &data, max_bytes_ + url.size() + 64) && data.compare(0, header_prefix.size(), header_prefix) == 0 &&
                    data.size() >= header_prefix.size() + url.size() + 1 &&
                    data.compare(header_prefix.size(), url.size(), url) == 0 && data[header_prefix.size() + url.size()] == '\n';
    if (!ok) {
        // Corrupt, truncated, or a different URL with the same hash: drop it (a hash collision simply
        // means the other URL loses its cache entry).
        bytes_ -= it->second->second;
        lru_.erase(it->second);
        index_.erase(it);
        ::unlink(full.c_str());
        return std::nullopt;
    }
    touch_locked(file);
    return data.substr(header_prefix.size() + url.size() + 1);
}

bool DiskCache::put(const std::string &url, const std::string &bytes) {
    if (!usable_ || bytes.size() > max_bytes_) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string file = path_for(url);
    const std::string full = dir_ + "/" + file;
    const std::string tmp = full + ".tmp";
    const std::string header = "WNKIMG1 " + std::to_string(url.size()) + "\n" + url + "\n";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = true;
    for (const std::string *part : {&header, &bytes}) {
        const char *p = part->data();
        size_t left = part->size();
        while (ok && left > 0) {
            const ssize_t n = ::write(fd, p, left);
            if (n < 0 && errno == EINTR) continue;
            ok = n > 0;
            if (ok) {
                p += n;
                left -= static_cast<size_t>(n);
            }
        }
    }
    ::close(fd);
    // A cache needs atomic replacement, not durability: rename without fsync.
    if (!ok || ::rename(tmp.c_str(), full.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    auto it = index_.find(file);
    if (it != index_.end()) {
        bytes_ -= it->second->second;
        lru_.erase(it->second);
        index_.erase(it);
    }
    const uint64_t size = header.size() + bytes.size();
    lru_.push_front({file, size});
    index_[file] = lru_.begin();
    bytes_ += size;
    evict_locked(max_bytes_);
    return true;
}

void DiskCache::evict_locked(uint64_t target) {
    while (bytes_ > target && !lru_.empty()) {
        const auto &[file, size] = lru_.back();
        ::unlink((dir_ + "/" + file).c_str());
        bytes_ -= size;
        index_.erase(file);
        lru_.pop_back();
    }
}

uint64_t DiskCache::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

size_t DiskCache::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.size();
}

void DiskCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    evict_locked(0);
}

}  // namespace wnk::image
