// SPDX-License-Identifier: GPL-3.0-or-later
//
// IPTV channel lists: the extended M3U files providers hand out, one entry per channel. These are
// not HLS playlists, though both start with #EXTM3U: a channel's URL is what the player opens (an
// HLS or DASH manifest, a TS stream), with the headers that entry asks for. A list that turns out to
// be an HLS media playlist (#EXT-X- tags) is refused; AdaptiveSource reads those. Pure: no I/O.
//
// Tags above a URL line apply to that entry only:
//   #EXTINF:<duration> [attributes],<title>   tvg-id, tvg-logo, group-title, and key material:
//                                             keyid (kid, drm-keyid) + key (drm-key) as "kid:key"
//   #EXTVLCOPT:<name>=<value>                 a header, "http-" prefix dropped (http-referrer)
//   #EXTHTTP:{"<name>":"<value>",...}         headers
//   #KODIPROP:inputstream.adaptive.license_key=<value>
//                                             an http(s) value is a ClearKey licence URL; anything
//                                             else is key material ("kid:key" or a JWK set)
// After the URL, "|name=value" parameters (further ones separated by '|' or '&') are headers too and
// are stripped from the URL. Header names are canonicalised (referer and referrer → Referer,
// user-agent → User-Agent, origin, cookie, authorization); other names are kept as written. Long URL
// lines may wrap: every non-tag line up to the next '#' line is part of the URL.
//
// keys_text is passed to parse_content_keys() (wnk_media_ffmpeg) by the app; the headers go into
// AdaptiveSourceOptions::headers, usually after normalize_playback_headers(). See
// playback_headers.hpp for what the transport does with them (redirects, no cookie jar).
//
// Header values and keys are secrets as often as not: never log them.
#pragma once

#include "wnk/net/http.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace wnk::media {

struct IptvChannel {
    std::string title;
    std::string url;
    std::string group;
    std::string tvg_id;
    std::string logo;
    net::Headers headers;
    std::string keys_text;    // "kid:key" pairs or a JWK set, for parse_content_keys(); may be empty
    std::string license_url;  // a ClearKey licence server; may be empty
};

// False (with a reason) if `text` is not a channel list or has no playable entry. Entries without a
// URL are dropped.
bool parse_iptv_playlist(std::string_view text, std::vector<IptvChannel> *out, std::string *error);

}  // namespace wnk::media
