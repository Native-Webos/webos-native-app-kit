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

render::TextureId ImageTextures::find_frame(const std::string &key, size_t frame) {
    auto it = map_.find(key);
    if (it == map_.end() || it->second.shown != frame) return render::kNoTexture;
    touch(it->second, key);
    return it->second.id;
}

render::TextureId ImageTextures::acquire(const std::string &key, const ImageRGBA &image) {
    return acquire_frame(key, image, 0);
}

render::TextureId ImageTextures::acquire_frame(const std::string &key, const ImageRGBA &image, size_t frame) {
    frame = frame < image.frame_count() ? frame : 0;
    const std::vector<uint8_t> &px = image.frame(frame);
    auto it = map_.find(key);
    if (it != map_.end()) {
        Slot &s = it->second;
        touch(s, key);
        if (s.shown == frame) return s.id;
        if (r_.update_texture(s.id, image.width, image.height, px.data())) {
            s.shown = frame;
            return s.id;
        }
        // No in-place update: a new texture for this frame.
        const render::TextureId id = r_.create_texture(image.width, image.height, px.data());
        if (id == render::kNoTexture) return s.id;  // keep showing the last frame
        r_.destroy_texture(s.id);
        s.id = id;
        s.shown = frame;
        return id;
    }
    const render::TextureId id = r_.create_texture(image.width, image.height, px.data());
    if (id == render::kNoTexture) return id;
    order_.push_front(key);
    map_[key] = Slot{id, px.size(), frame_, order_.begin(), frame};
    bytes_ += px.size();
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
