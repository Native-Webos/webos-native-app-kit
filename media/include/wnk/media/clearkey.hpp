// SPDX-License-Identifier: GPL-3.0-or-later
//
// ClearKey: content keys the app has in the clear, for MPEG Common Encryption (ISO/IEC 23001-7:
// 'cenc', 'cens', 'cbc1', 'cbcs') in MP4 and DASH. The bundled FFmpeg's MP4 demuxer decrypts each
// sample with them before the packet leaves the source, so the decoders (Starfish, NDL) receive clear
// data. This is not a DRM system: whoever has the keys can decrypt the content, and nothing on the TV
// protects them. Widevine and PlayReady content needs a licence from their own servers and cannot be
// played this way.
//
// Keys come from the app ("kid:key" pairs, as IPTV playlists carry them) or from a ClearKey licence
// server (W3C Encrypted Media Extensions, "Clear Key" section: POST {"kids":[...],"type":"temporary"}
// → a JSON Web Key Set), whose URL a DASH manifest may name (clearkey:Laurl, dashif:Laurl).
//
// Key IDs and keys are 16 bytes, held as 32 lowercase hex digits. Keys are secrets: never log them.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/net/http.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wnk::media {

using ContentKeys = std::map<std::string, std::string>;  // key ID → key, 32 lowercase hex digits each

// A key ID or key in any common spelling: 32 hex digits with or without dashes (a UUID), any case,
// or 22 characters of unpadded base64url (JWK). Returns "" when it is none of these.
std::string normalize_key_id(std::string_view text);

// Keys as an app or playlist gives them: "kid:key", several separated by ',' or whitespace ('='
// works as well as ':'), or a JSON Web Key Set ({"keys":[{"kty":"oct","kid":"...","k":"..."}]}).
// False with a reason when anything in it is malformed (nothing is added then).
bool parse_content_keys(std::string_view text, ContentKeys *out, std::string *error);

// The W3C ClearKey licence request for these key IDs, and the keys in a licence server's answer.
std::string clearkey_license_request(const std::vector<std::string> &key_ids);
bool parse_clearkey_license(std::string_view body, ContentKeys *out, std::string *error);

// Asks a ClearKey licence server for these key IDs. Blocking (run it on a worker or the playback
// thread). Adds what it got to *out; false with a reason when the server gave none of them.
bool fetch_clearkey_license(net::HttpTransport &http, const std::string &url, const net::Headers &headers,
                            const std::vector<std::string> &key_ids, const async::CancellationToken &token,
                            ContentKeys *out, std::string *error);

// FFmpeg's "decryption_keys" option value ("kid=key:kid=key").
std::string ffmpeg_decryption_keys(const ContentKeys &keys);

}  // namespace wnk::media
