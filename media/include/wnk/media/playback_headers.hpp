// SPDX-License-Identifier: GPL-3.0-or-later
//
// Request headers a stream needs, as IPTV providers hand them out: a Referer, Origin, User-Agent or
// Cookie the server checks before it serves the manifest and segments. The result goes into
// AdaptiveSourceOptions::headers (or HttpStreamOptions::headers), which reaches every request the
// source makes. Pure: no I/O.
//
// HTTP header names are case-insensitive, so the helpers here treat "Referer" and "referer" as one
// header; net::Headers itself is a plain ordered list that does no lookup or deduplication.
//
// Header values are credentials as often as not (cookies, tokens): never log them.
//
// Known limits of the transport these headers go to:
//   - A redirect re-sends the custom Referer, Origin and Cookie entries to the new location, even
//     when it is a different host.
//   - The default transport keeps no cookie jar: a Set-Cookie on the manifest response is not echoed
//     to the segment requests. A provider that needs that plays only through a transport made with
//     net::CurlOptions::cookies = true (AdaptiveSourceOptions::http).
#pragma once

#include "wnk/net/http.hpp"

#include <string>
#include <string_view>

namespace wnk::media {

// A desktop browser's User-Agent; many IPTV servers refuse unknown clients.
inline constexpr const char *kIptvUserAgent = "Mozilla/5.0 (Windows NT 10.0; rv:78.0) Gecko/20100101 Firefox/78.0";

// Sets `name` to `value`, replacing every header of that name in any case (the first one keeps its
// place and takes the new spelling).
void header_set(net::Headers *headers, std::string_view name, std::string_view value);
// The value of the first header named `name` in any case, or nullptr.
const std::string *header_get(const net::Headers &headers, std::string_view name);
// Removes every header named `name` in any case; false if there was none.
bool header_remove(net::Headers *headers, std::string_view name);

struct PlaybackLink {
    std::string url;
    net::Headers headers;
};

// "https://h/x.m3u8|Referer=https://a/|User-Agent=U": the URL up to the first '|', then one
// name=value header per '|'-separated part, split at its first '='. Names are kept as written; parts
// without a name are skipped.
PlaybackLink parse_playback_link(std::string_view link);

// "%2F" (either case) in the URL's path decoded to '/': some providers percent-encode the path
// separators. The query and fragment are left alone.
std::string decode_path_slashes(std::string_view url);

// Appends User-Agent: default_user_agent (unless it is empty) and Accept: */* when the headers name
// neither.
void normalize_playback_headers(net::Headers *headers, std::string_view default_user_agent);

}  // namespace wnk::media
