// SPDX-License-Identifier: GPL-3.0-or-later
//
// ImageSource for widgets backed by the kit's image service (fetch, decode, caches) and GPU texture
// cache. Main thread only. Both referenced objects must outlive this adapter, and the adapter must
// outlive the Ui that uses it.
#pragma once

#include "wnk/image/image.hpp"
#include "wnk/image/textures.hpp"
#include "wnk/ui/ui.hpp"

namespace wnk::ui {

class ServiceImageSource : public ImageSource {
 public:
    ServiceImageSource(image::ImageService &service, image::ImageTextures &textures)
        : service_(service), textures_(textures) {}

    Ticket load(const std::string &url, int max_w, int max_h, std::function<void(Loaded)> done) override;
    void cancel(Ticket ticket) override { service_.cancel(ticket); }
    render::TextureId texture(const std::string &key) override;

 private:
    image::ImageService &service_;
    image::ImageTextures &textures_;
    async::Lifetime lifetime_;
};

}  // namespace wnk::ui
