// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/ui/image_source.hpp"

namespace wnk::ui {

ImageSource::Ticket ServiceImageSource::load(const std::string &url, int max_w, int max_h,
                                             std::function<void(Loaded)> done) {
    return service_.request({url, max_w, max_h}, lifetime_.weak(), [done = std::move(done)](image::ImageResult r) {
        Loaded l;
        l.ok = r.error == image::ImageError::None && r.image != nullptr;
        l.key = r.key;
        if (r.image) {
            l.width = r.image->width;
            l.height = r.image->height;
            l.delays_ms = r.image->delays_ms;
            l.loops = r.image->loops;
        }
        if (!l.ok) l.error = std::string(image::error_name(r.error)) + (r.detail.empty() ? "" : ": " + r.detail);
        done(std::move(l));
    });
}

render::TextureId ServiceImageSource::texture(const std::string &key) { return texture(key, 0); }

render::TextureId ServiceImageSource::texture(const std::string &key, size_t frame) {
    if (const render::TextureId t = textures_.find_frame(key, frame); t != render::kNoTexture) return t;
    // Another frame needs the pixels, which the memory cache keeps while the animation shows. Evicted:
    // the frame on screen stays (still images: load again).
    const auto img = service_.memory().get(key);
    if (!img) return frame == 0 ? render::kNoTexture : textures_.find(key);
    return textures_.acquire_frame(key, *img, frame);
}

}  // namespace wnk::ui
