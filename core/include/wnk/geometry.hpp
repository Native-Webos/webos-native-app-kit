// SPDX-License-Identifier: GPL-3.0-or-later
//
// Basic 2D geometry shared by rendering, layout and focus. All values are in logical UI units
// (a 1920x1080 canvas) unless stated otherwise.
#pragma once

#include <algorithm>

namespace wnk {

struct Point {
    float x = 0, y = 0;
};

struct Size {
    float w = 0, h = 0;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(Point p) const { return p.x >= x && p.y >= y && p.x < right() && p.y < bottom(); }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }

    Rect intersect(const Rect &o) const {
        const float l = std::max(x, o.x), t = std::max(y, o.y);
        const float r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        return {l, t, std::max(0.0f, r - l), std::max(0.0f, b - t)};
    }

    bool operator==(const Rect &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(const Rect &o) const { return !(*this == o); }
};

}  // namespace wnk
