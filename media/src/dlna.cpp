// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/dlna.hpp"

#include "wnk/log.hpp"
#include "wnk/media/xml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace wnk::media {

namespace {

constexpr const char *kCat = "dlna";
constexpr size_t kMaxPageBytes = 4u << 20;

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string xml_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

bool to_int64(const std::string &s, int64_t *out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    char *end = nullptr;
    const long long v = std::strtoll(t.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    *out = v;
    return true;
}

int to_int(const std::string &s, int fallback = 0) {
    int64_t v = 0;
    return to_int64(s, &v) && v >= INT32_MIN && v <= INT32_MAX ? static_cast<int>(v) : fallback;
}

std::string child_text(const XmlNode &n, const char *local) {
    const XmlNode *c = n.child(local);
    return c != nullptr ? trim(c->text) : std::string();
}

// "http-get:*:video/mp4:DLNA.ORG_PN=...;DLNA.ORG_OP=01;..."
void read_protocol_info(DlnaResource *r) {
    std::vector<std::string> f;
    size_t i = 0;
    for (int k = 0; k < 3; ++k) {
        const size_t c = r->protocol_info.find(':', i);
        if (c == std::string::npos) break;
        f.push_back(r->protocol_info.substr(i, c - i));
        i = c + 1;
    }
    if (f.size() < 3) return;
    r->protocol = lower(trim(f[0]));
    r->mime_type = lower(trim(f[2]));
    const std::string extra = r->protocol_info.substr(i);
    size_t p = 0;
    while (p <= extra.size()) {
        size_t semi = extra.find(';', p);
        if (semi == std::string::npos) semi = extra.size();
        const std::string kv = extra.substr(p, semi - p);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string k = trim(kv.substr(0, eq)), v = trim(kv.substr(eq + 1));
            if (k == "DLNA.ORG_PN") r->dlna_profile = v;
            if (k == "DLNA.ORG_OP" && v.size() == 2) r->seekable = v[0] == '1' || v[1] == '1';
        }
        p = semi + 1;
    }
}

bool thumbnail(const DlnaResource &r) {
    return r.dlna_profile.find("_TN") != std::string::npos || r.dlna_profile.find("_SM_ICO") != std::string::npos ||
           r.dlna_profile.find("_LRG_ICO") != std::string::npos;
}

DlnaKind kind_of(const std::string &upnp_class, bool container) {
    if (container) return DlnaKind::Container;
    if (upnp_class.rfind("object.item.videoItem", 0) == 0) return DlnaKind::Video;
    if (upnp_class.rfind("object.item.audioItem", 0) == 0) return DlnaKind::Audio;
    if (upnp_class.rfind("object.item.imageItem", 0) == 0) return DlnaKind::Image;
    return DlnaKind::Other;
}

std::string upnp_error(const XmlNode &envelope) {
    const XmlNode *body = envelope.child("Body");
    const XmlNode *fault = body != nullptr ? body->child("Fault") : nullptr;
    if (fault == nullptr) return {};
    std::string code, desc;
    if (const XmlNode *detail = fault->child("detail")) {
        if (const XmlNode *e = detail->child("UPnPError")) {
            code = child_text(*e, "errorCode");
            desc = child_text(*e, "errorDescription");
        }
    }
    if (code.empty()) desc = child_text(*fault, "faultstring");
    static const struct {
        const char *code, *meaning;
    } kKnown[] = {{"401", "invalid action"}, {"402", "invalid arguments"}, {"701", "no such object"},
                  {"709", "unsupported sort criteria"}, {"720", "cannot process the request"}};
    if (desc.empty()) {
        for (const auto &k : kKnown) {
            if (code == k.code) desc = k.meaning;
        }
    }
    return "UPnP error " + (code.empty() ? std::string("?") : code) + (desc.empty() ? "" : ": " + desc);
}

}  // namespace

int64_t parse_dlna_duration(const std::string &text) {
    const std::string t = trim(text);
    const size_t c1 = t.find(':');
    const size_t c2 = c1 == std::string::npos ? std::string::npos : t.find(':', c1 + 1);
    if (c1 == std::string::npos || c2 == std::string::npos || c1 == 0) return kNoTime;
    int64_t h = 0, m = 0, s = 0;
    const std::string sec = t.substr(c2 + 1);
    const size_t dot = sec.find('.');
    if (!to_int64(t.substr(0, c1), &h) || !to_int64(t.substr(c1 + 1, c2 - c1 - 1), &m) ||
        !to_int64(sec.substr(0, dot), &s) || h < 0 || m < 0 || m > 59 || s < 0 || s > 59) {
        return kNoTime;
    }
    int64_t frac_us = 0;
    if (dot != std::string::npos) {
        const std::string f = sec.substr(dot + 1);
        const size_t slash = f.find('/');
        if (slash != std::string::npos) {  // F0/F1
            int64_t a = 0, b = 0;
            if (!to_int64(f.substr(0, slash), &a) || !to_int64(f.substr(slash + 1), &b) || b <= 0 || a < 0 || a >= b) return kNoTime;
            frac_us = a * 1000000 / b;
        } else {
            if (f.empty() || f.find_first_not_of("0123456789") != std::string::npos) return kNoTime;
            const std::string digits = (f + "000000").substr(0, 6);
            frac_us = std::atoll(digits.c_str());
        }
    }
    if (h > 1000000) return kNoTime;
    return ((h * 60 + m) * 60 + s) * 1000000 + frac_us;
}

const DlnaResource *DlnaObject::playable() const {
    if (kind == DlnaKind::Container) return nullptr;  // folders may offer themselves as playlists
    const char *want = kind == DlnaKind::Video ? "video/" : kind == DlnaKind::Audio ? "audio/" : kind == DlnaKind::Image ? "image/" : nullptr;
    const DlnaResource *best = nullptr;
    for (const DlnaResource &r : resources) {
        if (r.url.empty() || (r.protocol != "http-get" && !r.protocol.empty()) || thumbnail(r)) continue;
        if (r.mime_type.find("mpegurl") != std::string::npos) continue;  // an M3U playlist, not media
        if (want != nullptr && r.mime_type.rfind(want, 0) != 0) continue;
        if (want == nullptr && !r.mime_type.empty() && r.mime_type.rfind("video/", 0) != 0 &&
            r.mime_type.rfind("audio/", 0) != 0) {
            continue;
        }
        // The first that fits (servers list the original first), unless a later one is larger.
        if (best == nullptr || (static_cast<int64_t>(r.width) * r.height > static_cast<int64_t>(best->width) * best->height)) best = &r;
    }
    return best;
}

std::string DlnaObject::poster_url() const {
    if (!album_art_url.empty()) return album_art_url;
    const DlnaResource *any = nullptr;
    for (const DlnaResource &r : resources) {
        if (r.url.empty() || r.mime_type.rfind("image/", 0) != 0) continue;
        if (thumbnail(r)) return r.url;  // a thumbnail is what a poster wants
        if (any == nullptr) any = &r;
    }
    return any != nullptr ? any->url : std::string();
}

bool parse_didl_lite(const std::string &didl, const std::string &base_url, std::vector<DlnaObject> *out,
                     std::string *error) {
    out->clear();
    XmlNode root;
    XmlLimits limits;
    limits.max_bytes = kMaxPageBytes;
    std::string why;
    if (!parse_xml(didl, &root, &why, limits)) {
        if (error != nullptr) *error = "DIDL-Lite: " + why;
        return false;
    }
    if (root.local() != "DIDL-Lite") {
        if (error != nullptr) *error = "not DIDL-Lite (root <" + root.name + ">)";
        return false;
    }
    for (const XmlNode &n : root.children) {
        const std::string name = n.local();
        if (name != "container" && name != "item") continue;
        DlnaObject o;
        o.id = n.attribute_or("id", "");
        o.parent_id = n.attribute_or("parentID", "");
        const std::string restricted = lower(n.attribute_or("restricted", "0"));
        o.restricted = restricted == "1" || restricted == "true";
        o.child_count = to_int(n.attribute_or("childCount", ""), -1);
        o.title = child_text(n, "title");
        o.upnp_class = child_text(n, "class");
        o.kind = kind_of(o.upnp_class, name == "container");
        o.artist = child_text(n, "artist");
        if (o.artist.empty()) o.artist = child_text(n, "creator");
        o.album = child_text(n, "album");
        o.genre = child_text(n, "genre");
        o.date = child_text(n, "date");
        o.description = child_text(n, "description");
        if (o.description.empty()) o.description = child_text(n, "longDescription");
        const std::string art = child_text(n, "albumArtURI");
        if (!art.empty()) o.album_art_url = net::resolve_http_url(base_url, art);
        for (const XmlNode *res : n.children_named("res")) {
            DlnaResource r;
            r.url = trim(res->text);
            if (!r.url.empty()) r.url = net::resolve_http_url(base_url, r.url);
            r.protocol_info = res->attribute_or("protocolInfo", "");
            read_protocol_info(&r);
            int64_t size = -1;
            if (to_int64(res->attribute_or("size", ""), &size) && size >= 0) r.size = size;
            r.duration_us = parse_dlna_duration(res->attribute_or("duration", ""));
            const std::string resolution = res->attribute_or("resolution", "");
            const size_t x = resolution.find('x');
            if (x != std::string::npos) {
                r.width = to_int(resolution.substr(0, x));
                r.height = to_int(resolution.substr(x + 1));
            }
            r.bitrate = to_int(res->attribute_or("bitrate", ""));
            r.sample_rate = to_int(res->attribute_or("sampleFrequency", ""));
            r.channels = to_int(res->attribute_or("nrAudioChannels", ""));
            o.resources.push_back(std::move(r));
        }
        out->push_back(std::move(o));
    }
    return true;
}

std::string dlna_browse_request(const std::string &service_type, const std::string &object_id, bool metadata,
                                int start, int count, const std::string &sort) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
           "<u:Browse xmlns:u=\"" + xml_escape(service_type) + "\">"
           "<ObjectID>" + xml_escape(object_id) + "</ObjectID>"
           "<BrowseFlag>" + (metadata ? "BrowseMetadata" : "BrowseDirectChildren") + "</BrowseFlag>"
           "<Filter>*</Filter>"
           "<StartingIndex>" + std::to_string(std::max(0, start)) + "</StartingIndex>"
           "<RequestedCount>" + std::to_string(std::max(0, count)) + "</RequestedCount>"
           "<SortCriteria>" + xml_escape(sort) + "</SortCriteria>"
           "</u:Browse></s:Body></s:Envelope>";
}

bool parse_dlna_browse_response(const std::string &soap, const std::string &base_url, DlnaPage *out, std::string *error) {
    *out = DlnaPage{};
    XmlNode env;
    XmlLimits limits;
    limits.max_bytes = kMaxPageBytes;
    std::string why;
    if (!parse_xml(soap, &env, &why, limits)) {
        if (error != nullptr) *error = "SOAP: " + why;
        return false;
    }
    if (env.local() != "Envelope") {
        if (error != nullptr) *error = "not a SOAP envelope (root <" + env.name + ">)";
        return false;
    }
    const std::string fault = upnp_error(env);
    if (!fault.empty()) {
        if (error != nullptr) *error = fault;
        return false;
    }
    const XmlNode *body = env.child("Body");
    const XmlNode *resp = body != nullptr ? body->child("BrowseResponse") : nullptr;
    if (resp == nullptr) {
        if (error != nullptr) *error = "no BrowseResponse in the SOAP body";
        return false;
    }
    const XmlNode *result = resp->child("Result");
    // The DIDL-Lite document travels escaped (or in CDATA) as the Result's text.
    const std::string didl = result != nullptr ? trim(result->text) : std::string();
    if (!didl.empty() && !parse_didl_lite(didl, base_url, &out->objects, error)) return false;
    out->number_returned = to_int(child_text(*resp, "NumberReturned"), static_cast<int>(out->objects.size()));
    out->total_matches = to_int(child_text(*resp, "TotalMatches"));
    int64_t update = 0;
    if (to_int64(child_text(*resp, "UpdateID"), &update) && update >= 0) out->update_id = static_cast<uint32_t>(update);
    return true;
}

bool media_server_from_device(const net::UpnpDevice &device, const std::string &location, MediaServer *out) {
    for (const net::UpnpService &s : device.services) {
        if (s.type.rfind("urn:schemas-upnp-org:service:ContentDirectory:", 0) != 0 || s.control_url.empty()) continue;
        out->name = !device.friendly_name.empty() ? device.friendly_name : device.model_name;
        out->udn = device.udn;
        out->location = location;
        out->service_type = s.type;
        out->control_url = s.control_url;
        out->device = device;
        return true;
    }
    return false;
}

std::vector<MediaServer> find_media_servers(net::HttpTransport &http, std::chrono::milliseconds wait,
                                            const async::CancellationToken &token, std::string *error,
                                            const net::SsdpOptions &options) {
    std::vector<MediaServer> out;
    std::vector<std::string> seen;  // UDNs: a server may answer from several addresses
    for (const net::SsdpResponse &r :
         net::ssdp_search("urn:schemas-upnp-org:device:MediaServer:1", wait, token, error, options)) {
        if (token.cancelled()) break;
        net::UpnpDevice d;
        std::string why;
        if (!net::fetch_upnp_description(http, r.location, &d, token, &why)) {
            WNK_LOGW(kCat, "%s: %s", r.location.c_str(), why.c_str());
            continue;
        }
        MediaServer s;
        if (!media_server_from_device(d, r.location, &s)) {
            WNK_LOGW(kCat, "%s: a MediaServer without a ContentDirectory", r.location.c_str());
            continue;
        }
        if (s.name.empty()) s.name = r.address;
        if (!s.udn.empty() && std::find(seen.begin(), seen.end(), s.udn) != seen.end()) continue;
        seen.push_back(s.udn);
        out.push_back(std::move(s));
    }
    return out;
}

bool ContentDirectory::call(const std::string &object_id, bool metadata, int start, int count, DlnaPage *out,
                            const async::CancellationToken &token, std::string *error) {
    net::HttpRequest req;
    req.method = "POST";
    req.url = server_.control_url;
    req.headers = {{"Content-Type", "text/xml; charset=\"utf-8\""},
                   {"SOAPACTION", "\"" + server_.service_type + "#Browse\""}};
    req.body = dlna_browse_request(server_.service_type, object_id, metadata, start, count);
    req.timeout_ms = 15000;
    req.connect_timeout_ms = 3000;
    req.max_response_bytes = kMaxPageBytes;
    req.idempotent = true;  // Browse reads only
    const net::HttpResponse r = http_.perform(req, token);
    if (r.error != net::NetError::None) {
        if (error != nullptr) *error = std::string(net::error_name(r.error)) + (r.detail.empty() ? "" : ": " + r.detail);
        return false;
    }
    // A fault comes as HTTP 500 with a SOAP body that says why.
    if (!r.ok() && r.status != 500) {
        if (error != nullptr) *error = "HTTP " + std::to_string(r.status);
        return false;
    }
    std::string why;
    if (!parse_dlna_browse_response(r.body, server_.control_url, out, &why)) {
        if (error != nullptr) *error = r.ok() ? why : "HTTP " + std::to_string(r.status) + ": " + why;
        return false;
    }
    if (!r.ok()) {
        if (error != nullptr) *error = "HTTP " + std::to_string(r.status);
        return false;
    }
    return true;
}

bool ContentDirectory::browse(const std::string &object_id, int start, int count, DlnaPage *out,
                              const async::CancellationToken &token, std::string *error) {
    return call(object_id, false, start, count, out, token, error);
}

bool ContentDirectory::browse_all(const std::string &object_id, std::vector<DlnaObject> *out,
                                  const async::CancellationToken &token, std::string *error, int page, size_t limit) {
    out->clear();
    page = std::max(1, page);
    while (out->size() < limit) {
        DlnaPage p;
        const int want = static_cast<int>(std::min<size_t>(static_cast<size_t>(page), limit - out->size()));
        if (!call(object_id, false, static_cast<int>(out->size()), want, &p, token, error)) return false;  // *out keeps what came
        const size_t got = p.objects.size();
        for (DlnaObject &o : p.objects) {
            if (out->size() >= limit) break;
            out->push_back(std::move(o));
        }
        // Done on an empty page, at TotalMatches when the server knows it, or on a short page when
        // it does not (TotalMatches 0).
        if (got == 0) break;
        if (p.total_matches > 0 && out->size() >= static_cast<size_t>(p.total_matches)) break;
        if (p.total_matches == 0 && got < static_cast<size_t>(want)) break;
    }
    return true;
}

bool ContentDirectory::metadata(const std::string &object_id, DlnaObject *out, const async::CancellationToken &token,
                                std::string *error) {
    DlnaPage p;
    if (!call(object_id, true, 0, 0, &p, token, error)) return false;
    if (p.objects.empty()) {
        if (error != nullptr) *error = "no object in the BrowseMetadata result";
        return false;
    }
    *out = std::move(p.objects.front());
    return true;
}

}  // namespace wnk::media
