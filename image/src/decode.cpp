// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/image/image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <cstdio>
#include <cstring>
#include "nanosvg.h"
#include "nanosvgrast.h"

#include <webp/decode.h>
#include <webp/demux.h>

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
    if (b.size() >= 10 && (b.compare(0, 6, "GIF87a") == 0 || b.compare(0, 6, "GIF89a") == 0)) {
        *w = static_cast<int>(u8(6) | (u8(7) << 8));
        *h = static_cast<int>(u8(8) | (u8(9) << 8));
        return true;
    }
    if (b.size() >= 26 && b.compare(0, 2, "BM") == 0) {
        const auto le32 = [&](size_t i) { return static_cast<int32_t>(u8(i) | (u8(i + 1) << 8) | (u8(i + 2) << 16) | (u8(i + 3) << 24)); };
        const int32_t bw = le32(18), bh = le32(22);  // a negative height means top-down rows
        *w = bw < 0 ? 0 : bw;
        *h = bh == INT32_MIN ? 0x7FFFFFFF : std::abs(bh);
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

DecodeResult too_large(int w, int h) {
    DecodeResult r;
    r.error = ImageError::TooLarge;
    r.detail = "image is " + std::to_string(w) + "x" + std::to_string(h);
    return r;
}

DecodeResult failed(std::string why) {
    DecodeResult r;
    r.error = ImageError::Decode;
    r.detail = std::move(why);
    return r;
}

// The raster image fitted inside max_w x max_h (never enlarged), every frame of an animation.
void finish(DecodeResult &r, std::shared_ptr<ImageRGBA> img, int max_w, int max_h) {
    int fw = img->width, fh = img->height;
    fit_size(img->width, img->height, max_w, max_h, &fw, &fh);
    if (fw != img->width || fh != img->height) {
        auto out = std::make_shared<ImageRGBA>(downscale(*img, fw, fh));
        out->delays_ms = img->delays_ms;
        out->loops = img->loops;
        ImageRGBA one;
        one.width = img->width;
        one.height = img->height;
        for (auto &f : img->more_frames) {
            one.pixels = std::move(f);
            out->more_frames.push_back(downscale(one, fw, fh).pixels);
        }
        img = std::move(out);
    }
    r.image = std::move(img);
}

// Formats count a delay this short as "as fast as possible"; browsers show it for 100 ms.
int frame_delay(int ms) { return ms < 20 ? 100 : ms; }

bool animation_fits(int w, int h, size_t frames, const DecodeLimits &limits) {
    return limits.animate && frames > 1 && static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4 * frames <= limits.max_animation_bytes;
}

// GIF structure without decoding: the logical screen, how many images it holds and how many times
// to play it: once without a NETSCAPE2.0 block, forever for its count 0, else count + 1 (the count
// is of repeats, as browsers read it). False if the block structure is broken before the first image.
bool gif_scan(const std::string &b, int *w, int *h, size_t *frames, int *loops) {
    const auto *p = reinterpret_cast<const uint8_t *>(b.data());
    const size_t n = b.size();
    if (n < 13) return false;
    *w = p[6] | (p[7] << 8);
    *h = p[8] | (p[9] << 8);
    *frames = 0;
    *loops = 1;
    size_t pos = 13;
    if (p[10] & 0x80) pos += 3u << ((p[10] & 7) + 1);  // global colour table
    auto skip_sub_blocks = [&]() {
        while (pos < n) {
            const uint8_t len = p[pos++];
            if (len == 0) return true;
            pos += len;
        }
        return false;
    };
    while (pos < n) {
        const uint8_t tag = p[pos++];
        if (tag == 0x3B) break;  // trailer
        if (tag == 0x21) {       // extension
            if (pos >= n) break;
            const uint8_t label = p[pos++];
            if (label == 0xFF && pos + 16 <= n && p[pos] == 11 && std::memcmp(p + pos + 1, "NETSCAPE2.0", 11) == 0 &&
                p[pos + 12] == 3 && p[pos + 13] == 1) {
                const int repeats = p[pos + 14] | (p[pos + 15] << 8);
                *loops = repeats == 0 ? 0 : repeats + 1;
            }
            if (!skip_sub_blocks()) break;
        } else if (tag == 0x2C) {  // image descriptor
            if (pos + 9 > n) break;
            const uint8_t flags = p[pos + 8];
            pos += 9;
            if (flags & 0x80) pos += 3u << ((flags & 7) + 1);  // local colour table
            ++pos;                                             // LZW minimum code size
            if (!skip_sub_blocks()) break;
            ++*frames;
        } else {
            break;
        }
    }
    return *frames > 0 || *w > 0;
}

// Every frame of an animated GIF, composited by stb (disposal, transparency).
DecodeResult decode_gif_animation(const std::string &bytes, int loops, int max_w, int max_h) {
    int *delays = nullptr;
    int w = 0, h = 0, z = 0, comp = 0;
    stbi_uc *px = stbi_load_gif_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()), static_cast<int>(bytes.size()),
                                            &delays, &w, &h, &z, &comp, 4);
    if (px == nullptr) {
        STBI_FREE(delays);
        return failed(std::string("unreadable gif (") + (stbi_failure_reason() != nullptr ? stbi_failure_reason() : "?") + ")");
    }
    auto img = std::make_shared<ImageRGBA>();
    img->width = w;
    img->height = h;
    img->loops = loops;
    const size_t frame = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    for (int i = 0; i < z; ++i) {
        const stbi_uc *f = px + frame * static_cast<size_t>(i);
        if (i == 0) {
            img->pixels.assign(f, f + frame);
        } else {
            img->more_frames.emplace_back(f, f + frame);
        }
        img->delays_ms.push_back(frame_delay(delays != nullptr ? delays[i] : 100));
    }
    stbi_image_free(px);
    STBI_FREE(delays);
    if (z < 2) {
        img->delays_ms.clear();
        img->more_frames.clear();
    }
    DecodeResult r;
    finish(r, std::move(img), max_w, max_h);
    return r;
}

// Every frame of an animated WebP on its canvas (libwebp's WebPAnimDecoder blends and disposes).
DecodeResult decode_webp_animation(const std::string &bytes, int max_w, int max_h) {
    WebPAnimDecoderOptions o;
    if (!WebPAnimDecoderOptionsInit(&o)) return failed("WebP animation decoder unavailable");
    o.color_mode = MODE_RGBA;
    o.use_threads = 0;
    const WebPData data{reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()};
    WebPAnimDecoder *dec = WebPAnimDecoderNew(&data, &o);
    if (dec == nullptr) return failed("animated WebP did not decode");
    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(dec, &info)) {
        WebPAnimDecoderDelete(dec);
        return failed("animated WebP without information");
    }
    auto img = std::make_shared<ImageRGBA>();
    img->width = static_cast<int>(info.canvas_width);
    img->height = static_cast<int>(info.canvas_height);
    img->loops = static_cast<int>(info.loop_count);
    const size_t frame = static_cast<size_t>(img->width) * static_cast<size_t>(img->height) * 4;
    int previous = 0;
    while (WebPAnimDecoderHasMoreFrames(dec)) {
        uint8_t *buf = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp)) {
            WebPAnimDecoderDelete(dec);
            return failed("WebP frame decoding failed");
        }
        if (img->delays_ms.empty()) {
            img->pixels.assign(buf, buf + frame);
        } else {
            img->more_frames.emplace_back(buf, buf + frame);
        }
        img->delays_ms.push_back(frame_delay(timestamp - previous));  // timestamps are when each frame ends
        previous = timestamp;
    }
    WebPAnimDecoderDelete(dec);
    if (img->delays_ms.empty()) return failed("animated WebP without frames");
    if (img->delays_ms.size() < 2) img->delays_ms.clear();
    DecodeResult r;
    finish(r, std::move(img), max_w, max_h);
    return r;
}

// SVG: parsed, then rasterised at the size it will be shown (fitted to max_w x max_h, which may enlarge
// it: a vector icon stays sharp at any size), else at its own size.
DecodeResult decode_svg(const std::string &bytes, int max_w, int max_h, const DecodeLimits &limits) {
    std::string text = bytes;  // nanosvg parses in place
    NSVGimage *svg = nsvgParse(text.data(), "px", 96.0f);
    if (svg == nullptr) return failed("SVG did not parse");
    const float w = svg->width, h = svg->height;
    if (!(w > 0 && h > 0) || svg->shapes == nullptr) {
        nsvgDelete(svg);
        return failed("SVG without a size or without shapes");
    }
    float scale = 1;
    if (max_w > 0 || max_h > 0) {
        const float sx = max_w > 0 ? static_cast<float>(max_w) / w : 1e9f;
        const float sy = max_h > 0 ? static_cast<float>(max_h) / h : 1e9f;
        scale = std::min(sx, sy);
    }
    const int ow = std::max(1, static_cast<int>(std::lround(w * scale)));
    const int oh = std::max(1, static_cast<int>(std::lround(h * scale)));
    if (!within(ow, oh, limits)) {
        nsvgDelete(svg);
        return too_large(ow, oh);
    }
    auto img = std::make_shared<ImageRGBA>();
    img->width = ow;
    img->height = oh;
    img->pixels.assign(static_cast<size_t>(ow) * static_cast<size_t>(oh) * 4, 0);
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (rast == nullptr) {
        nsvgDelete(svg);
        return failed("SVG rasteriser out of memory");
    }
    nsvgRasterize(rast, svg, 0, 0, scale, img->pixels.data(), ow, oh, ow * 4);  // straight alpha out
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);
    DecodeResult r;
    r.image = std::move(img);
    return r;
}

// WebP: lossy, lossless, with alpha; of an animation, the first frame placed on its canvas.
DecodeResult decode_webp(const std::string &bytes, int max_w, int max_h, const DecodeLimits &limits) {
    const auto *data = reinterpret_cast<const uint8_t *>(bytes.data());
    WebPBitstreamFeatures f;
    if (WebPGetFeatures(data, bytes.size(), &f) != VP8_STATUS_OK) return failed("not a readable WebP image");
    if (!within(f.width, f.height, limits)) return too_large(f.width, f.height);
    auto img = std::make_shared<ImageRGBA>();
    if (!f.has_animation) {
        int w = 0, h = 0;
        uint8_t *px = WebPDecodeRGBA(data, bytes.size(), &w, &h);
        if (px == nullptr) return failed("WebP decoding failed");
        img->width = w;
        img->height = h;
        img->pixels.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        WebPFree(px);
    } else {
        WebPData whole{data, bytes.size()};
        WebPDemuxer *demux = WebPDemux(&whole);
        if (demux == nullptr) return failed("animated WebP did not demux");
        const int cw = static_cast<int>(WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH));
        const int ch = static_cast<int>(WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT));
        const size_t count = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
        if (within(cw, ch, limits) && animation_fits(cw, ch, count, limits)) {
            WebPDemuxDelete(demux);
            return decode_webp_animation(bytes, max_w, max_h);
        }
        WebPIterator it;
        if (!within(cw, ch, limits) || !WebPDemuxGetFrame(demux, 1, &it)) {
            WebPDemuxDelete(demux);
            return within(cw, ch, limits) ? failed("animated WebP without frames") : too_large(cw, ch);
        }
        int w = 0, h = 0;
        uint8_t *px = WebPDecodeRGBA(it.fragment.bytes, it.fragment.size, &w, &h);
        img->width = cw;
        img->height = ch;
        img->pixels.assign(static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4, 0);
        if (px != nullptr) {
            for (int y = 0; y < h && it.y_offset + y < ch; ++y) {
                const int n = std::min(w, cw - it.x_offset);
                if (n <= 0) break;
                std::memcpy(&img->pixels[(static_cast<size_t>(it.y_offset + y) * cw + it.x_offset) * 4],
                            px + static_cast<size_t>(y) * w * 4, static_cast<size_t>(n) * 4);
            }
            WebPFree(px);
        }
        WebPDemuxReleaseIterator(&it);
        WebPDemuxDelete(demux);
        if (px == nullptr) return failed("WebP frame decoding failed");
    }
    DecodeResult r;
    finish(r, std::move(img), max_w, max_h);
    return r;
}

}  // namespace

const char *sniff_format(const std::string &b) {
    static const char kPng[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
    if (b.size() >= 8 && b.compare(0, 8, kPng, 8) == 0) return "png";
    if (b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xD8 &&
        static_cast<unsigned char>(b[2]) == 0xFF) {
        return "jpeg";
    }
    if (b.size() >= 6 && (b.compare(0, 6, "GIF87a") == 0 || b.compare(0, 6, "GIF89a") == 0)) return "gif";
    if (b.size() >= 26 && b.compare(0, 2, "BM") == 0) return "bmp";
    if (b.size() >= 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) return "webp";
    // SVG is text: an <svg element near the start, after an optional BOM, XML declaration, comments or
    // DOCTYPE. A gzip-compressed .svgz is not read.
    const size_t window = std::min<size_t>(b.size(), 4096);
    const std::string head = b.substr(0, window);
    const size_t svg = head.find("<svg");
    if (svg != std::string::npos) {
        const size_t first = head.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
        if (first != std::string::npos && head[first] == '<') return "svg";
    }
    return "";
}

DecodeResult decode_image(const std::string &bytes, int max_w, int max_h, const DecodeLimits &limits) {
    DecodeResult r;
    if (bytes.empty() || bytes.size() > limits.max_encoded_bytes) {
        r.error = bytes.empty() ? ImageError::Decode : ImageError::TooLarge;
        r.detail = bytes.empty() ? "empty data" : "encoded image exceeds limit";
        return r;
    }
    const std::string format = sniff_format(bytes);
    if (format == "svg") return decode_svg(bytes, max_w, max_h, limits);
    if (format == "webp") return decode_webp(bytes, max_w, max_h, limits);
    if (format.empty()) return failed("not a PNG, JPEG, GIF, BMP, WebP or SVG image");
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
                             : std::string("unreadable ") + format + " (" + (why != nullptr ? why : "?") + ")";
        return r;
    }
    if (!within(w, h, limits)) {
        r.error = ImageError::TooLarge;
        r.detail = "image is " + std::to_string(w) + "x" + std::to_string(h);
        return r;
    }
    if (format == "gif") {
        int gw = 0, gh = 0, loops = 0;
        size_t frames = 0;
        if (gif_scan(bytes, &gw, &gh, &frames, &loops) && animation_fits(w, h, frames, limits)) {
            return decode_gif_animation(bytes, loops, max_w, max_h);
        }
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
    finish(r, std::move(img), max_w, max_h);
    return r;
}

}  // namespace wnk::image
