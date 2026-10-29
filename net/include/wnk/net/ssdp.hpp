// SPDX-License-Identifier: GPL-3.0-or-later
//
// UPnP devices on the home network: SSDP discovery (UPnP Device Architecture 2.0, section 1) and the
// device description each one publishes. This finds DLNA media servers, routers (IGD), speakers and
// bridges that do not announce themselves over mDNS (wnk/net/lan.hpp).
//
// ssdp_search sends M-SEARCH to the SSDP group from an ordinary UDP port; devices answer by unicast
// to that port, so the app joins no multicast group and never binds port 1900, which the TV's own
// UPnP stack holds. It blocks: run it on a worker. The description is plain HTTP(S) through the
// kit's transport.
//
// Not here: DIAL (launching apps on another screen). The TV's own DIAL server only lists apps LG
// registers, so a Developer Mode app can neither be launched through it nor stand in for it.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/net/http.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace wnk::net {

// One answer to an M-SEARCH (an HTTP/1.1 200 over UDP).
struct SsdpResponse {
    std::string address;   // IPv4 the answer came from
    std::string location;  // LOCATION: the device description URL
    std::string st;        // ST: the search target it matched
    std::string usn;       // USN: "uuid:...::urn:..." (one device answers once per type it has)
    std::string server;    // SERVER: "OS/version UPnP/1.0 product/version"
    int max_age_s = 0;     // CACHE-CONTROL max-age, 0 if absent
    std::vector<std::pair<std::string, std::string>> headers;  // every header, names lower-cased
    std::string header(const std::string &name) const;         // case-insensitive, or empty
    std::string uuid() const;  // the device's "uuid:..." part of USN
};

struct SsdpOptions {
    std::string address = "239.255.255.250";  // the SSDP group (a test points it at a local responder)
    uint16_t port = 1900;
    int mx = 2;  // seconds devices may wait before answering (1..5)
};

// "ssdp:all", "upnp:rootdevice", "urn:schemas-upnp-org:device:MediaServer:1", "uuid:...".
std::string ssdp_search_request(const std::string &st, const SsdpOptions &options = {});
// False if `data` is not an HTTP/1.1 200 with a LOCATION.
bool ssdp_parse_response(const char *data, size_t size, SsdpResponse *out);
// Searches for `st` and collects answers for `wait` (or until cancelled), one per USN, in arrival
// order. The request goes out twice (UDP may lose one).
std::vector<SsdpResponse> ssdp_search(const std::string &st, std::chrono::milliseconds wait,
                                      const async::CancellationToken &token = {}, std::string *error = nullptr,
                                      const SsdpOptions &options = {});

struct UpnpService {
    std::string type;  // "urn:schemas-upnp-org:service:ContentDirectory:1"
    std::string id;
    std::string control_url, event_url, scpd_url;  // absolute
};

// The root device of a description document (urn:schemas-upnp-org:device-1-0). Embedded devices
// are not separated: their services are listed with the root's.
struct UpnpDevice {
    std::string device_type;   // "urn:schemas-upnp-org:device:MediaServer:1"
    std::string friendly_name;
    std::string manufacturer, model_name, model_number, serial_number;
    std::string udn;               // "uuid:..."
    std::string presentation_url;  // absolute, or empty
    std::vector<UpnpService> services;
};

// Reads a description; relative URLs resolve against URLBase, else `location`.
bool parse_upnp_description(const std::string &xml, const std::string &location, UpnpDevice *out, std::string *error);
// GETs `location` (at most 256 KiB) and parses it.
bool fetch_upnp_description(HttpTransport &http, const std::string &location, UpnpDevice *out,
                            const async::CancellationToken &token = {}, std::string *error = nullptr);

// `ref` against `base` (RFC 3986 for the forms descriptions use: absolute, //host, /path, relative).
std::string resolve_http_url(const std::string &base, const std::string &ref);

}  // namespace wnk::net
