// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/render/renderer.hpp"

#include <algorithm>
#include <cmath>

namespace wnk::render {

CanvasMapping CanvasMapping::fit(Size logical, int drawable_w, int drawable_h) {
    CanvasMapping m;
    m.logical = logical;
    m.drawable_w = drawable_w;
    m.drawable_h = drawable_h;
    if (logical.w <= 0 || logical.h <= 0 || drawable_w <= 0 || drawable_h <= 0) return m;
    m.scale = std::min(drawable_w / logical.w, drawable_h / logical.h);
    m.offset_x = std::floor((drawable_w - logical.w * m.scale) * 0.5f);
    m.offset_y = std::floor((drawable_h - logical.h * m.scale) * 0.5f);
    return m;
}

}  // namespace wnk::render
