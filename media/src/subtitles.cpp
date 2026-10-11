// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/subtitles.hpp"

#include <algorithm>
#include <cstdlib>

namespace wnk::media {
namespace {

uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t content_hash(const SubtitleCue &c) {
    uint64_t h = fnv(14695981039346656037ull, c.text.data(), c.text.size());
    for (const SubtitleBitmap &b : c.bitmaps) {
        const int geom[4] = {b.x, b.y, b.width, b.height};
        h = fnv(h, geom, sizeof geom);
        h = fnv(h, b.rgba.data(), b.rgba.size());
    }
    return h;
}

size_t bitmap_size(const SubtitleCue &c) {
    size_t n = 0;
    for (const SubtitleBitmap &b : c.bitmaps) n += b.rgba.size();
    return n;
}

// Trims spaces at both ends of every line and drops empty lines.
std::string tidy_lines(const std::string &s) {
    std::string out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos) end = s.size();
        size_t a = start, b = end;
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
        if (b > a) {
            if (!out.empty()) out += '\n';
            out.append(s, a, b - a);
        }
        start = end + 1;
    }
    return out;
}

}  // namespace

std::string ass_event_text(const std::string &event) {
    // The text is everything after the eighth comma.
    size_t pos = 0;
    for (int commas = 0; commas < 8; ++commas) {
        pos = event.find(',', pos);
        if (pos == std::string::npos) return {};
        ++pos;
    }
    const std::string text = event.substr(pos);
    std::string out;
    bool drawing = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{') {
            const size_t close = text.find('}', i);
            if (close == std::string::npos) {  // unbalanced: keep the brace as text
                if (!drawing) out += c;
                continue;
            }
            // \pN turns drawing mode on (N > 0) or off (0) for the following text.
            const std::string block = text.substr(i + 1, close - i - 1);
            for (size_t t = block.find("\\p"); t != std::string::npos; t = block.find("\\p", t + 2)) {
                const char *num = block.c_str() + t + 2;
                if (*num >= '0' && *num <= '9') drawing = std::atoi(num) > 0;
            }
            i = close;
            continue;
        }
        if (c == '\\' && i + 1 < text.size()) {
            const char n = text[i + 1];
            if (n == 'N' || n == 'n') {
                out += '\n';
                ++i;
                continue;
            }
            if (n == 'h') {
                out += ' ';
                ++i;
                continue;
            }
        }
        if (!drawing) out += c;
    }
    return tidy_lines(out);
}

bool SubtitleStore::add(std::shared_ptr<const SubtitleCue> cue) {
    if (!cue || cue->start_us == kNoTime) return false;
    const uint64_t hash = content_hash(*cue);
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Entry> &cues = tracks_[cue->track];
    auto it = std::lower_bound(cues.begin(), cues.end(), cue->start_us,
                               [](const Entry &e, int64_t t) { return e.cue->start_us < t; });
    for (auto j = it; j != cues.end() && j->cue->start_us == cue->start_us; ++j) {
        if (j->hash == hash) return false;  // read again after a seek
    }
    const size_t bytes = bitmap_size(*cue);
    cues.insert(it, Entry{std::move(cue), hash, bytes});
    bitmap_bytes_ += bytes;
    evict_locked();
    return true;
}

void SubtitleStore::evict_locked() {
    auto distance = [this](const Entry &e) {
        const int64_t d = e.cue->start_us - last_pos_us_;
        return d < 0 ? -d : d;
    };
    for (auto &[track, cues] : tracks_) {
        while (cues.size() > limits_.max_cues_per_track) {
            auto far = std::max_element(cues.begin(), cues.end(),
                                        [&](const Entry &a, const Entry &b) { return distance(a) < distance(b); });
            bitmap_bytes_ -= far->bytes;
            cues.erase(far);
        }
    }
    while (bitmap_bytes_ > limits_.max_bitmap_bytes) {
        std::vector<Entry> *owner = nullptr;
        std::vector<Entry>::iterator far;
        int64_t worst = -1;
        for (auto &[track, cues] : tracks_) {
            for (auto it = cues.begin(); it != cues.end(); ++it) {
                if (it->bytes == 0) continue;
                const int64_t d = distance(*it);
                if (d > worst) {
                    worst = d;
                    owner = &cues;
                    far = it;
                }
            }
        }
        if (owner == nullptr) break;
        bitmap_bytes_ -= far->bytes;
        owner->erase(far);
    }
}

std::vector<std::shared_ptr<const SubtitleCue>> SubtitleStore::active(int track, int64_t pos_us) const {
    std::vector<std::shared_ptr<const SubtitleCue>> out;
    if (pos_us == kNoTime) return out;
    std::lock_guard<std::mutex> lock(mutex_);
    last_pos_us_ = pos_us;
    const auto t = tracks_.find(track);
    if (t == tracks_.end()) return out;
    const std::vector<Entry> &cues = t->second;
    // Candidates start at or before pos; walk back a bounded number of them (long cues overlap few).
    auto upper = std::upper_bound(cues.begin(), cues.end(), pos_us,
                                  [](int64_t p, const Entry &e) { return p < e.cue->start_us; });
    int budget = 64;
    for (auto it = upper; it != cues.begin() && budget-- > 0;) {
        --it;
        const SubtitleCue &c = *it->cue;
        int64_t end = c.end_us;
        if (end == kNoTime) {
            // Until the next cue that starts later (PGS clear events included).
            auto next = it + 1;
            while (next != cues.end() && next->cue->start_us == c.start_us) ++next;
            end = next != cues.end() ? next->cue->start_us : c.start_us + limits_.open_end_us;
            end = std::min(end, c.start_us + limits_.open_end_us);
        }
        if (pos_us < end && !c.empty()) out.push_back(it->cue);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void SubtitleStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_.clear();
    bitmap_bytes_ = 0;
}

size_t SubtitleStore::count(int track) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto t = tracks_.find(track);
    return t == tracks_.end() ? 0 : t->second.size();
}

size_t SubtitleStore::bitmap_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bitmap_bytes_;
}

}  // namespace wnk::media
