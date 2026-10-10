// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace wnk::render {

// Straight (non-premultiplied) RGBA, 0..1.
struct Color {
    float r = 0, g = 0, b = 0, a = 1;

    // 0xRRGGBBAA
    static constexpr Color hex(uint32_t rgba) {
        return {((rgba >> 24) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
                (rgba & 0xFF) / 255.0f};
    }
    constexpr Color with_alpha(float alpha) const { return {r, g, b, alpha}; }
};

inline constexpr Color kTransparent{0, 0, 0, 0};
inline constexpr Color kWhite{1, 1, 1, 1};
inline constexpr Color kBlack{0, 0, 0, 1};

}  // namespace wnk::render
