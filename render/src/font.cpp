// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/render/font.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

namespace {

constexpr size_t kMaxFontBytes = 64u << 20;  // refuse absurd inputs

// A read-only mapping of a whole file: pages are read on first use and are clean, so a large CJK
// face costs the memory of the glyphs actually drawn, not of the file.
struct Mapping {
    const uint8_t *data = nullptr;
    size_t size = 0;
    Mapping() = default;
    Mapping(const Mapping &) = delete;
    Mapping &operator=(const Mapping &) = delete;
    ~Mapping() {
        if (data != nullptr) munmap(const_cast<uint8_t *>(data), size);
    }
    bool open(const std::string &path, std::string *error) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (error != nullptr) *error = "cannot open " + path;
            return false;
        }
        struct stat st {};
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
            static_cast<uint64_t>(st.st_size) > kMaxFontBytes) {
            ::close(fd);
            if (error != nullptr) *error = path + " is not a regular file of at most 64 MiB";
            return false;
        }
        void *p = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) {
            if (error != nullptr) *error = "cannot map " + path;
            return false;
        }
        data = static_cast<const uint8_t *>(p);
        size = static_cast<size_t>(st.st_size);
        return true;
    }
};

}  // namespace

struct FontFace::Impl {
    std::vector<uint8_t> data;              // from_bytes
    std::unique_ptr<Mapping> mapping;       // from_file
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
    auto map = std::make_unique<Mapping>();
    if (!map->open(path, error)) return nullptr;
    auto face = std::unique_ptr<FontFace>(new FontFace());
    face->impl_ = std::make_unique<Impl>();
    const int offset = stbtt_GetFontOffsetForIndex(map->data, 0);
    if (offset < 0 || static_cast<size_t>(offset) >= map->size || !stbtt_InitFont(&face->impl_->info, map->data, offset)) {
        if (error != nullptr) *error = path + ": not a TrueType or OpenType font";
        return nullptr;
    }
    face->impl_->mapping = std::move(map);
    return face;
}

bool FontFace::covers(char32_t codepoint) const { return glyph_index(codepoint) != 0; }

// ---- FontFinder -----------------------------------------------------------------------------------

namespace {

bool font_file_name(const std::string &name) {
    std::string lower = name;
    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto ends = [&lower](const char *suffix) {
        const size_t n = std::char_traits<char>::length(suffix);
        return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
    };
    return ends(".ttf") || ends(".otf") || ends(".ttc");
}

void walk_fonts(const std::string &dir, int depth, std::vector<std::string> *out) {
    if (depth > 6 || out->size() > 4000) return;
    DIR *d = opendir(dir.c_str());
    if (d == nullptr) return;
    std::vector<std::string> names;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] != '.') names.emplace_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const auto &n : names) {
        const std::string path = dir + "/" + n;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_fonts(path, depth + 1, out);
        } else if (S_ISREG(st.st_mode) && font_file_name(n)) {
            out->push_back(path);
        }
    }
}

}  // namespace

FontFinder::FontFinder(std::vector<std::string> dirs) {
    for (const auto &d : dirs) walk_fonts(d, 0, &files_);
}

int FontFinder::score(const std::string &path, const std::vector<std::string> &preferred) {
    std::string name = path.substr(path.find_last_of('/') + 1);
    int s = 0;
    for (const auto &tag : preferred) {
        if (!tag.empty() && name.find(tag) != std::string::npos) s += 100;
    }
    std::string lower = name;
    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char *w : {"bold", "black", "heavy", "light", "thin", "medium", "semi", "italic", "oblique", "mono",
                          "serif", "condensed", "narrow", "emoji", "symbol", "math"}) {
        if (lower.find(w) != std::string::npos) s -= 10;
    }
    if (lower.find("regular") != std::string::npos) s += 5;
    return s;
}

std::vector<std::string> FontFinder::find(const std::vector<char32_t> &codepoints, const std::vector<std::string> &preferred) const {
    std::vector<std::pair<int, size_t>> order;
    for (size_t i = 0; i < files_.size(); ++i) order.emplace_back(-score(files_[i], preferred), i);
    std::stable_sort(order.begin(), order.end());
    std::set<char32_t> left(codepoints.begin(), codepoints.end());
    std::vector<std::string> chosen;
    for (const auto &[neg, i] : order) {
        if (left.empty()) break;
        Mapping map;
        if (!map.open(files_[i], nullptr)) continue;
        stbtt_fontinfo info{};
        const int offset = stbtt_GetFontOffsetForIndex(map.data, 0);
        if (offset < 0 || static_cast<size_t>(offset) >= map.size || !stbtt_InitFont(&info, map.data, offset)) continue;
        bool useful = false;
        for (auto it = left.begin(); it != left.end();) {
            if (stbtt_FindGlyphIndex(&info, static_cast<int>(*it)) != 0) {
                useful = true;
                it = left.erase(it);
            } else {
                ++it;
            }
        }
        if (useful) chosen.push_back(files_[i]);
    }
    return chosen;
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
