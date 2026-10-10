// SPDX-License-Identifier: GPL-3.0-or-later
//
// GPU textures for decoded images, owned through the renderer and bounded by a byte budget.
// Main thread only (it calls the renderer). Textures used in the current frame are never evicted.
#pragma once

#include "wnk/image/image.hpp"
#include "wnk/render/renderer.hpp"

#include <list>
#include <string>
#include <unordered_map>

namespace wnk::image {

class ImageTextures {
 public:
    ImageTextures(render::Renderer &renderer, size_t budget_bytes) : r_(renderer), budget_(budget_bytes) {}
    ~ImageTextures() { clear(); }
    ImageTextures(const ImageTextures &) = delete;
    ImageTextures &operator=(const ImageTextures &) = delete;

    // Texture for `key`, uploading `image` if needed; marks it used this frame. kNoTexture on failure.
    render::TextureId acquire(const std::string &key, const ImageRGBA &image);
    // Texture already uploaded for `key` (marks it used), else kNoTexture.
    render::TextureId find(const std::string &key);
    // Call once per frame after drawing: evicts least-recently-used textures beyond the budget.
    void end_frame();
    void clear();

    size_t bytes() const { return bytes_; }
    size_t count() const { return map_.size(); }

 private:
    struct Slot {
        render::TextureId id;
        size_t bytes;
        uint64_t last_frame;
        std::list<std::string>::iterator pos;
    };
    void touch(Slot &s, const std::string &key);
    render::Renderer &r_;
    size_t budget_;
    size_t bytes_ = 0;
    uint64_t frame_ = 1;
    std::list<std::string> order_;  // front = most recent
    std::unordered_map<std::string, Slot> map_;
};

}  // namespace wnk::image
