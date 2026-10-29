// SPDX-License-Identifier: GPL-3.0-or-later
//
// DLNA / UPnP media servers: finding them (SSDP, wnk/net/ssdp.hpp) and browsing what they share
// through their ContentDirectory service (UPnP ContentDirectory:1-4, Browse; DIDL-Lite results).
// A NAS, a router with a USB disk, Plex or Jellyfin in DLNA mode, Rygel, MiniDLNA, Windows Media
// Player all answer the same way. The media itself is plain HTTP: give a resource's URL to the
// player (AdaptiveSource / FfmpegSource) or an ImageView.
//
// Everything here blocks on the network: call it from a worker (async::run_then_post). Responses
// are bounded (4 MiB per page) and parsed without resolving external entities.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/media/media.hpp"
#include "wnk/net/http.hpp"
#include "wnk/net/ssdp.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace wnk::media {

// One <res> of an item: a URL and what it holds. A server may list several (the original, a
// transcode, a thumbnail, a subtitle file).
struct DlnaResource {
    std::string url;            // absolute
    std::string protocol_info;  // "http-get:*:video/mp4:DLNA.ORG_PN=AVC_MP4_HP_HD_AAC;DLNA.ORG_OP=01"
    std::string protocol;       // its first field ("http-get")
    std::string mime_type;      // its third field, lower case ("video/mp4")
    std::string dlna_profile;   // DLNA.ORG_PN, if given ("JPEG_TN" marks a thumbnail)
    bool seekable = false;      // DLNA.ORG_OP: byte (x1) or time (1x) seeking offered
    int64_t size = -1;          // bytes, -1 if not given
    int64_t duration_us = kNoTime;
    int width = 0, height = 0;  // resolution="1920x1080"
    int bitrate = 0;            // bytes per second, as UPnP defines it
    int sample_rate = 0, channels = 0;
};

enum class DlnaKind : uint8_t { Container, Video, Audio, Image, Other };

// A <container> (folder) or <item> of a DIDL-Lite document.
struct DlnaObject {
    std::string id, parent_id;
    DlnaKind kind = DlnaKind::Other;
    std::string upnp_class;  // "object.container.storageFolder", "object.item.videoItem.movie"
    std::string title;       // dc:title
    bool restricted = false;
    int child_count = -1;    // containers: childCount, -1 if not given
    std::string artist, album, genre, date, description;  // upnp:artist, upnp:album, dc:date, ...
    std::string album_art_url;  // upnp:albumArtURI, absolute; empty if none
    std::vector<DlnaResource> resources;

    // The resource to play or show: an http-get one whose type fits the item (video/, audio/,
    // image/), not a thumbnail; the largest picture among images and videos. nullptr if none.
    const DlnaResource *playable() const;
    // A picture for a poster: album art, else an image resource (a thumbnail one first). Empty if none.
    std::string poster_url() const;
};

struct DlnaPage {
    std::vector<DlnaObject> objects;
    int number_returned = 0;
    int total_matches = 0;  // 0 can also mean "not known" (some servers): see Browse below
    uint32_t update_id = 0;
};

// The SOAP body of a Browse request: BrowseDirectChildren of `object_id` ("0" is the root), or
// BrowseMetadata of the object itself.
std::string dlna_browse_request(const std::string &service_type, const std::string &object_id, bool metadata,
                                int start, int count, const std::string &sort = {});
// A Browse response envelope; a SOAP fault gives its UPnP errorCode and description in *error.
bool parse_dlna_browse_response(const std::string &soap, const std::string &base_url, DlnaPage *out, std::string *error);
// A DIDL-Lite document (the Result of a Browse). Relative URLs resolve against `base_url`.
bool parse_didl_lite(const std::string &didl, const std::string &base_url, std::vector<DlnaObject> *out,
                     std::string *error);
// "H+:MM:SS[.F+]" or "H+:MM:SS.F0/F1" in microseconds, kNoTime if malformed.
int64_t parse_dlna_duration(const std::string &text);

// A server and its ContentDirectory, from a device description.
struct MediaServer {
    std::string name;          // friendlyName, else modelName, else the address
    std::string udn;           // "uuid:..."
    std::string location;      // description URL
    std::string service_type;  // "urn:schemas-upnp-org:service:ContentDirectory:1"
    std::string control_url;   // absolute
    net::UpnpDevice device;
};
// False if the device has no ContentDirectory service.
bool media_server_from_device(const net::UpnpDevice &device, const std::string &location, MediaServer *out);

// Finds media servers: an SSDP search for MediaServer devices, then each description (in arrival
// order, one per device). Devices that fail to describe themselves are skipped with a warning.
std::vector<MediaServer> find_media_servers(net::HttpTransport &http, std::chrono::milliseconds wait,
                                            const async::CancellationToken &token = {}, std::string *error = nullptr,
                                            const net::SsdpOptions &options = {});

// Browse calls to one server's ContentDirectory.
class ContentDirectory {
 public:
    ContentDirectory(net::HttpTransport &http, MediaServer server) : http_(http), server_(std::move(server)) {}
    const MediaServer &server() const { return server_; }

    // One page of `object_id`'s children from `start` (at most `count`; servers may return fewer).
    bool browse(const std::string &object_id, int start, int count, DlnaPage *out,
                const async::CancellationToken &token = {}, std::string *error = nullptr);
    // All children, page by page (`page` per request) up to `limit` objects: until a page comes
    // back empty or TotalMatches is reached. Returns what it got so far on a later page's failure
    // with *error set.
    bool browse_all(const std::string &object_id, std::vector<DlnaObject> *out, const async::CancellationToken &token = {},
                    std::string *error = nullptr, int page = 200, size_t limit = 10000);
    // The object itself (BrowseMetadata), e.g. to refresh one item's resources.
    bool metadata(const std::string &object_id, DlnaObject *out, const async::CancellationToken &token = {},
                  std::string *error = nullptr);

 private:
    bool call(const std::string &object_id, bool metadata, int start, int count, DlnaPage *out,
              const async::CancellationToken &token, std::string *error);
    net::HttpTransport &http_;
    MediaServer server_;
};

}  // namespace wnk::media
