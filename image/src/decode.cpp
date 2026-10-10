// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/image/image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace wnk::image {

const char *error_name(ImageError e) {
    switch (e) {
        case ImageError::None: return "none";
        case ImageError::Network: return "network";
        case ImageError::HttpStatus: return "http-status";
        case ImageError::NotFound: return "not-found";
        case ImageError::Decode: return "decode";
        case ImageError::TooLarge: return "too-large";
        case ImageError::Cancelled: return "cancelled";
        case ImageError::Unsupported: return "unsupported";
    }
    return "?";
}

void fit_size(int w, int h, int max_w, int max_h, int *out_w, int *out_h) {
    double scale = 1.0;
    if (max_w > 0 && w > max_w) scale = std::min(scale, static_cast<double>(max_w) / w);
    if (max_h > 0 && h > max_h) scale = std::min(scale, static_cast<double>(max_h) / h);
    *out_w = std::max(1, static_cast<int>(std::lround(w * scale)));
    *out_h = std::max(1, static_cast<int>(std::lround(h * scale)));
    *out_w = std::min(*out_w, w);
    *out_h = std::min(*out_h, h);
}

ImageRGBA downscale(const ImageRGBA &src, int dst_w, int dst_h) {
    ImageRGBA out;
    out.width = dst_w;
    out.height = dst_h;
    out.pixels.assign(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 4, 0);
    const double sx = static_cast<double>(src.width) / dst_w;
    const double sy = static_cast<double>(src.height) / dst_h;
    for (int y = 0; y < dst_h; ++y) {
        const double y0 = y * sy, y1 = (y + 1) * sy;
        for (int x = 0; x < dst_w; ++x) {
            const double x0 = x * sx, x1 = (x + 1) * sx;
            // Area average over the source footprint, premultiplied by alpha so transparent pixels do not
            // bleed their colour into edges.
            double acc[4] = {0, 0, 0, 0}, area = 0;
            for (int yy = static_cast<int>(y0); yy < std::min(src.height, static_cast<int>(std::ceil(y1))); ++yy) {
                const double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
                for (int xx = static_cast<int>(x0); xx < std::min(src.width, static_cast<int>(std::ceil(x1))); ++xx) {
                    const double w = wy * (std::min<double>(xx + 1, x1) - std::max<double>(xx, x0));
                    const uint8_t *p = &src.pixels[(static_cast<size_t>(yy) * src.width + xx) * 4];
                    const double a = p[3] / 255.0;
                    acc[0] += p[0] * a * w;
                    acc[1] += p[1] * a * w;
                    acc[2] += p[2] * a * w;
                    acc[3] += p[3] * w;
                    area += w;
                }
            }
            uint8_t *q = &out.pixels[(static_cast<size_t>(y) * dst_w + x) * 4];
            const double alpha = area > 0 ? acc[3] / area : 0;
            for (int c = 0; c < 3; ++c) {
                const double v = alpha > 0 ? acc[c] / area / (alpha / 255.0) : 0;
                q[c] = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
            }
            q[3] = static_cast<uint8_t>(std::clamp(std::lround(alpha), 0L, 255L));
        }
    }
    return out;
}

namespace {

// Dimensions straight from the PNG IHDR or the JPEG SOF header, without stb: the size limits are then
// enforced before any decoder runs (stb's own guard reports "unknown image type" once it falls through
// to the next format, which hides the reason).
bool peek_dimensions(const std::string &b, int *w, int *h) {
    auto u8 = [&](size_t i) { return static_cast<uint32_t>(static_cast<unsigned char>(b[i])); };
    static const char kPng[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
    if (b.size() >= 24 && b.compare(0, 8, kPng, 8) == 0 && b.compare(12, 4, "IHDR") == 0) {
        const uint32_t pw = (u8(16) << 24) | (u8(17) << 16) | (u8(18) << 8) | u8(19);
        const uint32_t ph = (u8(20) << 24) | (u8(21) << 16) | (u8(22) << 8) | u8(23);
        *w = pw > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(pw);
        *h = ph > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(ph);
        return true;
    }
    if (b.size() >= 4 && u8(0) == 0xFF && u8(1) == 0xD8) {
        size_t i = 2;
        while (i + 4 <= b.size()) {
            if (u8(i) != 0xFF) return false;
            const uint32_t marker = u8(i + 1);
            if (marker == 0xFF) {  // fill byte
                ++i;
                continue;
            }
            if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
                i += 2;
                continue;
            }
            const size_t len = (u8(i + 2) << 8) | u8(i + 3);
            const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
            if (sof && i + 9 <= b.size()) {
                *h = static_cast<int>((u8(i + 5) << 8) | u8(i + 6));
                *w = static_cast<int>((u8(i + 7) << 8) | u8(i + 8));
                return true;
            }
            if (len < 2) return false;
            i += 2 + len;
        }
    }
    return false;
}

bool within(int w, int h, const DecodeLimits &limits) {
    return w > 0 && h > 0 && w <= limits.max_dimension && h <= limits.max_dimension &&
           static_cast<uint64_t>(w) * static_cast<uint64_t>(h) <= limits.max_pixels;
}

}  // namespace

DecodeResult decode_image(const std::string &bytes, int max_w, int max_h, const DecodeLimits &limits) {
    DecodeResult r;
    if (bytes.empty() || bytes.size() > limits.max_encoded_bytes) {
        r.error = bytes.empty() ? ImageError::Decode : ImageError::TooLarge;
        r.detail = bytes.empty() ? "empty data" : "encoded image exceeds limit";
        return r;
    }
    int w = 0, h = 0, comp = 0;
    if (peek_dimensions(bytes, &w, &h) && !within(w, h, limits)) {
        r.error = ImageError::TooLarge;
        r.detail = "image is " + std::to_string(w) + "x" + std::to_string(h);
        return r;
    }
    const auto *data = reinterpret_cast<const stbi_uc *>(bytes.data());
    const int len = static_cast<int>(bytes.size());
    // Header only: reject huge images before allocating pixels.
    if (!stbi_info_from_memory(data, len, &w, &h, &comp)) {
        // stb's own STBI_MAX_DIMENSIONS guard fails the header read with "too large".
        const char *why = stbi_failure_reason();
        const bool too_large = why != nullptr && std::string(why) == "too large";
        r.error = too_large ? ImageError::TooLarge : ImageError::Decode;
        r.detail = too_large ? "image dimensions exceed the decoder limit"
                             : std::string("not a PNG or JPEG image (") + (why != nullptr ? why : "?") + ")";
        return r;
    }
    if (!within(w, h, limits)) {
        r.error = ImageError::TooLarge;
        r.detail = "image is " + std::to_string(w) + "x" + std::to_string(h);
        return r;
    }
    stbi_uc *px = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (px == nullptr) {
        r.error = ImageError::Decode;
        r.detail = stbi_failure_reason() != nullptr ? stbi_failure_reason() : "decode failed";
        return r;
    }
    auto img = std::make_shared<ImageRGBA>();
    img->width = w;
    img->height = h;
    img->pixels.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(px);
    int fw = w, fh = h;
    fit_size(w, h, max_w, max_h, &fw, &fh);
    if (fw != w || fh != h) {
        img = std::make_shared<ImageRGBA>(downscale(*img, fw, fh));
    }
    r.image = std::move(img);
    return r;
}

}  // namespace wnk::image
