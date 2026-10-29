// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/ass_renderer.hpp"

#include "wnk/log.hpp"

#if WNK_HAVE_LIBASS
extern "C" {
#include <ass/ass.h>
}

#include <algorithm>
#include <chrono>
#include <cstdarg>
#endif

namespace wnk::media {

#if WNK_HAVE_LIBASS
namespace {

constexpr const char *kCat = "ass";

struct Box {
    int l, t, r, b;  // r and b exclusive
    bool near(const Box &o, int gap) const {
        return l - gap < o.r && o.l - gap < r && t - gap < o.b && o.t - gap < b;
    }
    void unite(const Box &o) {
        l = std::min(l, o.l);
        t = std::min(t, o.t);
        r = std::max(r, o.r);
        b = std::max(b, o.b);
    }
};

// Images close together share a region (one texture), so a line of glyphs with its outline and shadow
// is one upload, while a sign at the top and dialogue at the bottom stay two small ones.
std::vector<Box> group(std::vector<Box> boxes) {
    constexpr int kGap = 16;
    constexpr size_t kMaxRegions = 16;
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < boxes.size() && !merged; ++i) {
            for (size_t j = i + 1; j < boxes.size(); ++j) {
                if (!boxes[i].near(boxes[j], kGap)) continue;
                boxes[i].unite(boxes[j]);
                boxes.erase(boxes.begin() + static_cast<ptrdiff_t>(j));
                merged = true;
                break;
            }
        }
    }
    if (boxes.size() > kMaxRegions) {
        for (size_t i = 1; i < boxes.size(); ++i) boxes[0].unite(boxes[i]);
        boxes.resize(1);
    }
    return boxes;
}

void quiet(int level, const char *fmt, va_list args, void *) {
    if (level > 1) return;  // errors only (libass's 0 = fatal ... 7 = debug; 2 is font fallback notices)
    char buf[512];
    std::vsnprintf(buf, sizeof buf, fmt, args);
    WNK_LOGW(kCat, "libass: %s", buf);
}

class LibassRenderer final : public AssRenderer {
 public:
    LibassRenderer(ASS_Library *lib, ASS_Renderer *r, std::string default_font)
        : lib_(lib), renderer_(r), default_font_(std::move(default_font)) {}
    ~LibassRenderer() override {
        if (track_ != nullptr) ass_free_track(track_);
        ass_renderer_done(renderer_);
        ass_library_done(lib_);
    }

    void add_font(const std::string &name, const std::vector<uint8_t> &data) override {
        if (data.empty() || data.size() > (64u << 20)) return;
        ass_add_font(lib_, name.c_str(), reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()));
        fonts_changed_ = true;
    }

    bool set_track(const std::string &header) override {
        if (track_ != nullptr) ass_free_track(track_);
        track_ = ass_new_track(lib_);
        if (track_ == nullptr) return false;
        ass_process_codec_private(track_, header.data(), static_cast<int>(header.size()));
        ass_track_set_feature(track_, ASS_FEATURE_WRAP_UNICODE, 1);
        if (fonts_changed_ || !fonts_set_) {
            // Fonts added since are only seen by a new font selector.
            ass_set_fonts(renderer_, default_font_.empty() ? nullptr : default_font_.c_str(), "sans-serif",
                          ASS_FONTPROVIDER_NONE, nullptr, 0);
            fonts_set_ = true;
            fonts_changed_ = false;
        }
        width_ = height_ = 0;  // the next frame is new
        last_empty_ = false;
        return true;
    }

    void add_events(const SubtitleCue &cue) override {
        if (track_ == nullptr || cue.start_us == kNoTime) return;
        const long long start = cue.start_us / 1000;
        const long long duration = cue.end_us != kNoTime && cue.end_us > cue.start_us ? (cue.end_us - cue.start_us) / 1000 : 10000;
        for (const std::string &e : cue.ass_events) {
            ass_process_chunk(track_, e.data(), static_cast<int>(e.size()), start, duration);
        }
    }

    void prune(int64_t before_us) override {
        if (track_ != nullptr && before_us > 0) ass_prune_events(track_, before_us / 1000);
    }

    bool render(int64_t time_us, int width, int height, int storage_w, int storage_h, std::vector<Region> *regions,
                bool *changed) override {
        *changed = false;
        if (track_ == nullptr || width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
        const auto t0 = std::chrono::steady_clock::now();
        const bool resized = width != width_ || height != height_ || storage_w != storage_w_ || storage_h != storage_h_;
        if (resized) {
            ass_set_frame_size(renderer_, width, height);
            ass_set_storage_size(renderer_, std::max(0, storage_w), std::max(0, storage_h));
            width_ = width;
            height_ = height;
            storage_w_ = storage_w;
            storage_h_ = storage_h;
        }
        int detect = 0;
        ASS_Image *images = ass_render_frame(renderer_, track_, time_us / 1000, &detect);
        // libass reports a change on every empty frame; nothing after nothing is no change.
        if (!resized && (detect == 0 || (images == nullptr && last_empty_))) return true;
        last_empty_ = images == nullptr;
        *changed = true;
        regions->clear();
        std::vector<Box> boxes;
        for (ASS_Image *p = images; p != nullptr; p = p->next) {
            Box b{std::max(0, p->dst_x - 1), std::max(0, p->dst_y - 1), std::min(width, p->dst_x + p->w + 1),
                  std::min(height, p->dst_y + p->h + 1)};
            if (b.l < b.r && b.t < b.b && p->w > 0 && p->h > 0) boxes.push_back(b);
        }
        boxes = group(std::move(boxes));
        regions->resize(boxes.size());
        for (size_t i = 0; i < boxes.size(); ++i) {
            Region &g = (*regions)[i];
            g.x = boxes[i].l;
            g.y = boxes[i].t;
            g.width = boxes[i].r - boxes[i].l;
            g.height = boxes[i].b - boxes[i].t;
            g.rgba.assign(static_cast<size_t>(g.width) * g.height * 4, 0);
        }
        // Each image is a coverage mask in one colour, in drawing order (shadow, outline, glyph, next
        // event): composite "over" in premultiplied alpha, then convert to straight alpha.
        for (ASS_Image *p = images; p != nullptr; p = p->next) {
            const unsigned cr = p->color >> 24, cg = (p->color >> 16) & 0xFF, cb = (p->color >> 8) & 0xFF;
            const unsigned opacity = 255 - (p->color & 0xFF);
            if (opacity == 0) continue;
            for (Region &g : *regions) {
                const int l = std::max({p->dst_x, g.x, 0}), r = std::min({p->dst_x + p->w, g.x + g.width, width});
                const int t = std::max({p->dst_y, g.y, 0}), b = std::min({p->dst_y + p->h, g.y + g.height, height});
                if (l >= r || t >= b) continue;
                for (int y = t; y < b; ++y) {
                    const uint8_t *mask = p->bitmap + static_cast<ptrdiff_t>(y - p->dst_y) * p->stride + (l - p->dst_x);
                    uint8_t *dst = g.rgba.data() + (static_cast<size_t>(y - g.y) * g.width + (l - g.x)) * 4;
                    for (int x = l; x < r; ++x, ++mask, dst += 4) {
                        const unsigned a = (*mask * opacity + 127) / 255;
                        if (a == 0) continue;
                        const unsigned keep = 255 - a;
                        dst[0] = static_cast<uint8_t>((cr * a + dst[0] * keep + 127) / 255);
                        dst[1] = static_cast<uint8_t>((cg * a + dst[1] * keep + 127) / 255);
                        dst[2] = static_cast<uint8_t>((cb * a + dst[2] * keep + 127) / 255);
                        dst[3] = static_cast<uint8_t>(a + (dst[3] * keep + 127) / 255);
                    }
                }
                break;  // regions do not overlap
            }
        }
        for (Region &g : *regions) {
            for (size_t i = 0; i < g.rgba.size(); i += 4) {
                const unsigned a = g.rgba[i + 3];
                if (a == 0 || a == 255) continue;
                for (int c = 0; c < 3; ++c) g.rgba[i + c] = static_cast<uint8_t>(std::min(255u, (g.rgba[i + c] * 255u + a / 2) / a));
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++stats_.frames;
        stats_.last_ms = ms;
        stats_.max_ms = std::max(stats_.max_ms, ms);
        return true;
    }

    Stats stats() const override {
        Stats s = stats_;
        s.events = track_ != nullptr ? static_cast<size_t>(track_->n_events) : 0;
        return s;
    }

 private:
    ASS_Library *lib_;
    ASS_Renderer *renderer_;
    ASS_Track *track_ = nullptr;
    std::string default_font_;
    bool fonts_set_ = false, fonts_changed_ = false;
    int width_ = 0, height_ = 0, storage_w_ = 0, storage_h_ = 0;
    bool last_empty_ = false;
    Stats stats_;
};

}  // namespace

bool AssRenderer::available() { return true; }

std::unique_ptr<AssRenderer> AssRenderer::create(const std::string &default_font, std::string *error) {
    ASS_Library *lib = ass_library_init();
    if (lib == nullptr) {
        if (error != nullptr) *error = "ass_library_init failed";
        return nullptr;
    }
    ass_set_message_cb(lib, quiet, nullptr);
    ass_set_extract_fonts(lib, 1);
    ASS_Renderer *r = ass_renderer_init(lib);
    if (r == nullptr) {
        ass_library_done(lib);
        if (error != nullptr) *error = "ass_renderer_init failed";
        return nullptr;
    }
    ass_set_cache_limits(r, 1024, 32);  // glyphs, MiB of bitmaps: bounded for a 3 GB TV
    return std::make_unique<LibassRenderer>(lib, r, default_font);
}

#else

bool AssRenderer::available() { return false; }

std::unique_ptr<AssRenderer> AssRenderer::create(const std::string &, std::string *error) {
    if (error != nullptr) *error = "built without libass (WNK_LIBASS=OFF)";
    return nullptr;
}

#endif

}  // namespace wnk::media
