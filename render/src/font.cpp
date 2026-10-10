// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/render/font.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <cmath>
#include <cstdio>

namespace wnk::render {

std::vector<char32_t> decode_utf8(std::string_view s) {
    std::vector<char32_t> out;
    out.reserve(s.size());
    constexpr char32_t kReplacement = 0xFFFD;
    size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        int len;
        char32_t cp;
        if (b0 < 0x80) {
            out.push_back(b0);
            ++i;
            continue;
        } else if ((b0 & 0xE0) == 0xC0) {
            len = 2;
            cp = b0 & 0x1F;
        } else if ((b0 & 0xF0) == 0xE0) {
            len = 3;
            cp = b0 & 0x0F;
        } else if ((b0 & 0xF8) == 0xF0) {
            len = 4;
            cp = b0 & 0x07;
        } else {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        if (i + static_cast<size_t>(len) > s.size()) {
            out.push_back(kReplacement);
            break;
        }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            const auto b = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((b & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        static constexpr char32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (!ok) {
            out.push_back(kReplacement);
            ++i;  // resynchronise on the next byte
            continue;
        }
        if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
        out.push_back(cp);
        i += static_cast<size_t>(len);
    }
    return out;
}

struct FontFace::Impl {
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
};

FontFace::~FontFace() = default;

std::unique_ptr<FontFace> FontFace::from_bytes(std::vector<uint8_t> data, std::string *error) {
    auto face = std::unique_ptr<FontFace>(new FontFace());
    face->impl_ = std::make_unique<Impl>();
    face->impl_->data = std::move(data);
    const unsigned char *bytes = face->impl_->data.data();
    const int offset = face->impl_->data.empty() ? -1 : stbtt_GetFontOffsetForIndex(bytes, 0);
    if (offset < 0 || !stbtt_InitFont(&face->impl_->info, bytes, offset)) {
        if (error != nullptr) *error = "not a TrueType font";
        return nullptr;
    }
    return face;
}

std::unique_ptr<FontFace> FontFace::from_file(const std::string &path, std::string *error) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        if (error != nullptr) *error = "cannot open " + path;
        return nullptr;
    }
    std::vector<uint8_t> data;
    uint8_t buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        data.insert(data.end(), buf, buf + n);
        if (data.size() > (64u << 20)) {  // refuse absurd inputs
            std::fclose(f);
            if (error != nullptr) *error = path + " is larger than 64 MiB";
            return nullptr;
        }
    }
    std::fclose(f);
    auto face = from_bytes(std::move(data), error);
    if (face == nullptr && error != nullptr) *error = path + ": " + *error;
    return face;
}

FontMetrics FontFace::metrics(float pixel_size) const {
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&impl_->info, &ascent, &descent, &gap);
    const float scale = stbtt_ScaleForPixelHeight(&impl_->info, pixel_size);
    return {ascent * scale, -descent * scale, gap * scale};
}

int FontFace::glyph_index(char32_t codepoint) const {
    return stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(codepoint));
}

float FontFace::advance(int glyph, float pixel_size) const {
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&impl_->info, glyph, &adv, &lsb);
    return adv * stbtt_ScaleForPixelHeight(&impl_->info, pixel_size);
}

float FontFace::kerning(int left, int right, float pixel_size) const {
    return stbtt_GetGlyphKernAdvance(&impl_->info, left, right) * stbtt_ScaleForPixelHeight(&impl_->info, pixel_size);
}

GlyphBitmap FontFace::rasterize(int glyph, float pixel_size) const {
    GlyphBitmap out;
    const float scale = stbtt_ScaleForPixelHeight(&impl_->info, pixel_size);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&impl_->info, glyph, scale, scale, &x0, &y0, &x1, &y1);
    out.width = x1 - x0;
    out.height = y1 - y0;
    out.x_offset = x0;
    out.y_offset = y0;
    if (out.width <= 0 || out.height <= 0) {
        out.width = out.height = 0;
        return out;
    }
    out.alpha.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height), 0);
    stbtt_MakeGlyphBitmap(&impl_->info, out.alpha.data(), out.width, out.height, out.width, scale, scale, glyph);
    return out;
}

float FontFace::measure(std::string_view utf8, float pixel_size) const {
    float width = 0;
    int prev = 0;
    for (char32_t cp : decode_utf8(utf8)) {
        const int g = glyph_index(cp);
        if (prev != 0) width += kerning(prev, g, pixel_size);
        width += advance(g, pixel_size);
        prev = g;
    }
    return width;
}

ShelfPacker::Slot ShelfPacker::insert(int w, int h) {
    const int pw = w + padding_, ph = h + padding_;
    if (pw > width_ || ph > height_) return {};
    if (shelf_x_ + pw > width_ || ph > shelf_h_) {
        // Start a new shelf below the current one (also when the glyph is taller than this shelf).
        if (shelf_h_ != 0 || shelf_x_ != 0) {
            shelf_y_ += shelf_h_;
            shelf_x_ = 0;
            shelf_h_ = 0;
        }
    }
    if (shelf_h_ == 0) shelf_h_ = ph;
    if (shelf_y_ + shelf_h_ > height_) return {};
    Slot s{shelf_x_, shelf_y_, true};
    shelf_x_ += pw;
    return s;
}

void ShelfPacker::reset() { shelf_x_ = shelf_y_ = shelf_h_ = 0; }

}  // namespace wnk::render
