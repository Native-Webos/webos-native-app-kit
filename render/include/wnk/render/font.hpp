// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU-side font handling: TrueType parsing and glyph rasterisation (stb_truetype), UTF-8 decoding
// and text measurement. No GL here; the renderer owns the GPU glyph atlas.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wnk::render {

// Decode UTF-8. Invalid or truncated sequences become U+FFFD; overlong forms and surrogates are rejected.
std::vector<char32_t> decode_utf8(std::string_view text);

struct FontMetrics {
    float ascent = 0;    // pixels above the baseline (positive)
    float descent = 0;   // pixels below the baseline (positive)
    float line_gap = 0;
    float line_height() const { return ascent + descent + line_gap; }
};

struct GlyphBitmap {
    int width = 0, height = 0;
    int x_offset = 0, y_offset = 0;  // from the pen position on the baseline to the bitmap's top-left
    std::vector<uint8_t> alpha;      // width*height coverage values
};

class FontFace {
 public:
    // Takes ownership of the font file bytes. Returns nullptr if they are not a usable TrueType font.
    static std::unique_ptr<FontFace> from_bytes(std::vector<uint8_t> data, std::string *error = nullptr);
    // Maps the file read-only (TrueType, OpenType with CFF outlines, or the first face of a .ttc):
    // only the pages of glyphs actually used are read, which matters for 20 MB CJK faces.
    static std::unique_ptr<FontFace> from_file(const std::string &path, std::string *error = nullptr);
    ~FontFace();
    FontFace(const FontFace &) = delete;
    FontFace &operator=(const FontFace &) = delete;

    FontMetrics metrics(float pixel_size) const;
    int glyph_index(char32_t codepoint) const;  // 0 = .notdef (missing glyph)
    bool covers(char32_t codepoint) const;
    float advance(int glyph, float pixel_size) const;
    float kerning(int left_glyph, int right_glyph, float pixel_size) const;
    GlyphBitmap rasterize(int glyph, float pixel_size) const;

    // Width of a single line of text in pixels, including kerning.
    float measure(std::string_view utf8, float pixel_size) const;

 private:
    FontFace() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Finds font files that cover code points the app's fonts lack (the TV's own fonts, for example).
// The directories are listed once, at construction; find() opens candidates read-only and checks
// their character maps. Safe to use from a worker thread.
class FontFinder {
 public:
    explicit FontFinder(std::vector<std::string> dirs);
    // Files that together cover as many of `codepoints` as possible, best first. Candidates whose
    // file name contains a `preferred` tag come first (e.g. "JP" for Japanese forms of Han
    // characters); regular upright faces come before bold, light, italic, mono, serif or symbol ones.
    std::vector<std::string> find(const std::vector<char32_t> &codepoints, const std::vector<std::string> &preferred = {}) const;
    const std::vector<std::string> &files() const { return files_; }
    static int score(const std::string &path, const std::vector<std::string> &preferred);

 private:
    std::vector<std::string> files_;
};

// Shelf packer for a square-ish atlas. Rows are allocated top to bottom; each row is as tall as the
// first glyph placed in it (glyphs of one size are similar, which is the common case for UI text).
class ShelfPacker {
 public:
    ShelfPacker(int width, int height, int padding = 1) : width_(width), height_(height), padding_(padding) {}
    struct Slot {
        int x = 0, y = 0;
        bool ok = false;
    };
    Slot insert(int w, int h);
    void reset();
    int width() const { return width_; }
    int height() const { return height_; }
    int used_height() const { return shelf_y_ + shelf_h_; }

 private:
    int width_, height_, padding_;
    int shelf_x_ = 0, shelf_y_ = 0, shelf_h_ = 0;
};

}  // namespace wnk::render
