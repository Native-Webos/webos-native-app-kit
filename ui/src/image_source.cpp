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
        }
        if (!l.ok) l.error = std::string(image::error_name(r.error)) + (r.detail.empty() ? "" : ": " + r.detail);
        done(std::move(l));
    });
}

render::TextureId ServiceImageSource::texture(const std::string &key) {
    if (const render::TextureId t = textures_.find(key); t != render::kNoTexture) return t;
    const auto img = service_.memory().get(key);
    return img ? textures_.acquire(key, *img) : render::kNoTexture;
}

}  // namespace wnk::ui
