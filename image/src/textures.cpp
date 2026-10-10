// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/image/textures.hpp"

namespace wnk::image {

void ImageTextures::touch(Slot &s, const std::string &key) {
    s.last_frame = frame_;
    order_.splice(order_.begin(), order_, s.pos);
    (void)key;
}

render::TextureId ImageTextures::find(const std::string &key) {
    auto it = map_.find(key);
    if (it == map_.end()) return render::kNoTexture;
    touch(it->second, key);
    return it->second.id;
}

render::TextureId ImageTextures::acquire(const std::string &key, const ImageRGBA &image) {
    if (const render::TextureId id = find(key)) return id;
    const render::TextureId id = r_.create_texture(image.width, image.height, image.pixels.data());
    if (id == render::kNoTexture) return id;
    order_.push_front(key);
    map_[key] = Slot{id, image.bytes(), frame_, order_.begin()};
    bytes_ += image.bytes();
    return id;
}

void ImageTextures::end_frame() {
    while (bytes_ > budget_ && !order_.empty()) {
        auto it = map_.find(order_.back());
        if (it->second.last_frame == frame_) break;  // everything left was drawn this frame
        r_.destroy_texture(it->second.id);
        bytes_ -= it->second.bytes;
        map_.erase(it);
        order_.pop_back();
    }
    ++frame_;
}

void ImageTextures::clear() {
    for (auto &[key, s] : map_) r_.destroy_texture(s.id);
    map_.clear();
    order_.clear();
    bytes_ = 0;
}

}  // namespace wnk::image
