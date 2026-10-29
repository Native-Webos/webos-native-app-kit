// SPDX-License-Identifier: GPL-3.0-or-later
// DLNA ContentDirectory: Browse requests, SOAP and DIDL-Lite parsing (Rygel's real answers and a
// MiniDLNA-style one), durations, paging and faults through a scripted transport.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/dlna.hpp"

#include <cstdio>
#include <deque>
#include <fstream>
#include <sstream>

using namespace wnk;
using namespace wnk::media;

namespace {

std::string fixture(const std::string &name) {
    std::ifstream f(std::string(WNK_FIXTURES_DIR) + "/dlna/" + name, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

std::string envelope(const std::string &didl, int returned, int total, bool cdata = false) {
    return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
           "<u:BrowseResponse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\"><Result>" +
           (cdata ? "<![CDATA[" + didl + "]]>" : escape(didl)) + "</Result><NumberReturned>" + std::to_string(returned) +
           "</NumberReturned><TotalMatches>" + std::to_string(total) + "</TotalMatches><UpdateID>7</UpdateID>"
           "</u:BrowseResponse></s:Body></s:Envelope>";
}

std::string didl_of(int first, int n) {
    std::string d = "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
                    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">";
    for (int i = first; i < first + n; ++i) {
        d += "<item id=\"i" + std::to_string(i) + "\" parentID=\"64\" restricted=\"1\"><dc:title>Clip " + std::to_string(i) +
             "</dc:title><upnp:class>object.item.videoItem</upnp:class>"
             "<res protocolInfo=\"http-get:*:video/mp4:*\">/MediaItems/" + std::to_string(i) + ".mp4</res></item>";
    }
    return d + "</DIDL-Lite>";
}

const char *kFault =
    "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><s:Fault>"
    "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
    "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>701</errorCode>"
    "<errorDescription>No such object</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";

// Answers each perform() with the next scripted response and keeps the requests.
class ScriptedTransport final : public net::HttpTransport {
 public:
    std::deque<net::HttpResponse> answers;
    std::vector<net::HttpRequest> requests;
    net::HttpResponse perform(const net::HttpRequest &req, const async::CancellationToken &) override {
        requests.push_back(req);
        if (answers.empty()) {
            net::HttpResponse r;
            r.error = net::NetError::Connect;
            return r;
        }
        net::HttpResponse r = answers.front();
        answers.pop_front();
        return r;
    }
    std::string describe() const override { return "scripted"; }
    void add(long status, std::string body) {
        net::HttpResponse r;
        r.status = status;
        r.body = std::move(body);
        answers.push_back(std::move(r));
    }
};

MediaServer server() {
    MediaServer s;
    s.name = "Test server";
    s.service_type = "urn:schemas-upnp-org:service:ContentDirectory:1";
    s.control_url = "http://192.168.1.20:8200/ctl/ContentDir";
    return s;
}

std::string between(const std::string &s, const std::string &a, const std::string &b) {
    const size_t i = s.find(a);
    if (i == std::string::npos) return {};
    const size_t j = s.find(b, i + a.size());
    return j == std::string::npos ? std::string() : s.substr(i + a.size(), j - i - a.size());
}

}  // namespace

TEST("dlna: durations in H+:MM:SS[.F+] and F0/F1 forms") {
    CHECK_EQ(parse_dlna_duration("0:00:08.000"), int64_t{8000000});
    CHECK_EQ(parse_dlna_duration("1:02:03"), int64_t{3723000000});
    CHECK_EQ(parse_dlna_duration("0:45:12.5"), int64_t{2712500000});
    CHECK_EQ(parse_dlna_duration(" 10:00:00.123456789 "), int64_t{36000123456});
    CHECK_EQ(parse_dlna_duration("0:00:01.1/4"), int64_t{1250000});
    CHECK_EQ(parse_dlna_duration("0:60:00"), kNoTime);
    CHECK_EQ(parse_dlna_duration("0:00:61"), kNoTime);
    CHECK_EQ(parse_dlna_duration("12:34"), kNoTime);
    CHECK_EQ(parse_dlna_duration("0:00:01.4/4"), kNoTime);
    CHECK_EQ(parse_dlna_duration("-1:00:00"), kNoTime);
    CHECK_EQ(parse_dlna_duration(""), kNoTime);
}

TEST("dlna: the Browse request names its action and escapes the object id") {
    const std::string r = dlna_browse_request("urn:schemas-upnp-org:service:ContentDirectory:3", "a&b<c>", false, 20, 50);
    CHECK(r.find("<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:3\">") != std::string::npos);
    CHECK_EQ(between(r, "<ObjectID>", "</ObjectID>"), std::string("a&amp;b&lt;c&gt;"));
    CHECK_EQ(between(r, "<BrowseFlag>", "</BrowseFlag>"), std::string("BrowseDirectChildren"));
    CHECK_EQ(between(r, "<StartingIndex>", "</StartingIndex>"), std::string("20"));
    CHECK_EQ(between(r, "<RequestedCount>", "</RequestedCount>"), std::string("50"));
    const std::string m = dlna_browse_request("urn:x", "0", true, -3, -1);
    CHECK_EQ(between(m, "<BrowseFlag>", "</BrowseFlag>"), std::string("BrowseMetadata"));
    CHECK_EQ(between(m, "<StartingIndex>", "</StartingIndex>"), std::string("0"));
}

TEST("dlna: Rygel's root lists its folders") {
    DlnaPage p;
    std::string err;
    REQUIRE(parse_dlna_browse_response(fixture("rygel-root.xml"), "http://127.0.0.1:48200/Control/x", &p, &err));
    CHECK_EQ(p.number_returned, 5);
    CHECK_EQ(p.total_matches, 5);
    CHECK_EQ(p.update_id, uint32_t{29});
    REQUIRE(p.objects.size() == 5);
    const DlnaObject &fs = p.objects[0];
    CHECK_EQ(fs.id, std::string("Filesystem"));
    CHECK_EQ(fs.parent_id, std::string("0"));
    CHECK_EQ(fs.title, std::string("Files & Folders"));  // entity-escaped twice on the wire
    CHECK(fs.kind == DlnaKind::Container);
    CHECK_EQ(fs.child_count, 1);
    CHECK(fs.restricted);
    CHECK(fs.playable() == nullptr);  // its playlist resources are not media
    CHECK_EQ(p.objects[1].title, std::string("Music"));
}

TEST("dlna: Rygel's items: the HTTP resource of each kind, not its internal file") {
    DlnaPage p;
    std::string err;
    REQUIRE(parse_dlna_browse_response(fixture("rygel-films.xml"), "http://127.0.0.1:48200/Control/x", &p, &err));
    REQUIRE(p.objects.size() == 3);
    const DlnaObject &song = p.objects[0], &photo = p.objects[1], &film = p.objects[2];
    CHECK(song.kind == DlnaKind::Audio);
    CHECK(photo.kind == DlnaKind::Image);
    CHECK(film.kind == DlnaKind::Video);
    CHECK_EQ(film.title, std::string("Test Card"));
    CHECK_EQ(film.date, std::string("2026-10-29T21:27:59Z"));
    REQUIRE(film.resources.size() == 2);
    CHECK_EQ(film.resources[1].protocol, std::string("internal"));
    const DlnaResource *r = film.playable();
    REQUIRE(r != nullptr);
    CHECK(r->url.rfind("http://127.0.0.1:48200/MediaExport/i/", 0) == 0);
    CHECK(r->url.find("%3D/res/primary_http.mp4") != std::string::npos);
    CHECK_EQ(r->mime_type, std::string("video/mp4"));
    CHECK_EQ(r->size, int64_t{2847293});
    CHECK_EQ(r->duration_us, int64_t{8000000});
    CHECK_EQ(r->width, 1280);
    CHECK_EQ(r->height, 720);
    CHECK_EQ(r->channels, 2);
    CHECK_EQ(r->sample_rate, 48000);
    CHECK(r->seekable);
    const DlnaResource *a = song.playable();
    REQUIRE(a != nullptr);
    CHECK_EQ(a->mime_type, std::string("audio/mp4"));
    CHECK_EQ(a->dlna_profile, std::string("AAC_ISO_320"));
    CHECK_EQ(a->duration_us, int64_t{12000000});
    const DlnaResource *i = photo.playable();
    REQUIRE(i != nullptr);
    CHECK_EQ(i->mime_type, std::string("image/jpeg"));
    CHECK_EQ(photo.poster_url(), i->url);
}

TEST("dlna: MiniDLNA-style metadata, album art, thumbnails, relative URLs and a CDATA result") {
    const std::string didl =
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" xmlns:dlna=\"urn:schemas-dlna-org:metadata-1-0/\">"
        "<container id=\"64\" parentID=\"0\" restricted=\"1\" searchable=\"1\" childCount=\"12\"><dc:title>Browse Folders</dc:title>"
        "<upnp:class>object.container.storageFolder</upnp:class><upnp:storageUsed>-1</upnp:storageUsed></container>"
        "<item id=\"1$4$0\" parentID=\"1$4\" restricted=\"1\"><dc:title>Track &amp; Field</dc:title>"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class><dc:creator>An Artist</dc:creator>"
        "<upnp:album>An Album</upnp:album><upnp:genre>Jazz</upnp:genre>"
        "<upnp:albumArtURI dlna:profileID=\"JPEG_TN\">/AlbumArt/22-1.jpg</upnp:albumArtURI>"
        "<res size=\"5200000\" duration=\"0:04:12.533\" bitrate=\"40000\" sampleFrequency=\"44100\" nrAudioChannels=\"2\" "
        "protocolInfo=\"http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;DLNA.ORG_OP=01;DLNA.ORG_CI=0\">http://192.168.1.20:8200/MediaItems/22.mp3</res>"
        "</item><item id=\"2$8$1\" parentID=\"2$8\" restricted=\"1\"><dc:title>Holiday</dc:title>"
        "<upnp:class>object.item.videoItem</upnp:class>"
        "<res protocolInfo=\"http-get:*:image/jpeg:DLNA.ORG_PN=JPEG_TN\">/Thumbnails/31.jpg</res>"
        "<res size=\"900000000\" duration=\"1:30:00.000\" resolution=\"1920x1080\" protocolInfo=\"http-get:*:video/x-matroska:DLNA.ORG_OP=10\">"
        "/MediaItems/31.mkv</res><res resolution=\"640x360\" protocolInfo=\"http-get:*:video/mp4:*\">/MediaItems/31-small.mp4</res>"
        "</item><unknownElement/></DIDL-Lite>";
    DlnaPage p;
    std::string err;
    REQUIRE(parse_dlna_browse_response(envelope(didl, 3, 3, true), "http://192.168.1.20:8200/ctl/ContentDir", &p, &err));
    REQUIRE(p.objects.size() == 3);
    CHECK(p.objects[0].kind == DlnaKind::Container);
    CHECK_EQ(p.objects[0].child_count, 12);
    const DlnaObject &song = p.objects[1];
    CHECK_EQ(song.id, std::string("1$4$0"));
    CHECK_EQ(song.title, std::string("Track & Field"));
    CHECK_EQ(song.artist, std::string("An Artist"));  // dc:creator when upnp:artist is absent
    CHECK_EQ(song.album, std::string("An Album"));
    CHECK_EQ(song.genre, std::string("Jazz"));
    CHECK_EQ(song.poster_url(), std::string("http://192.168.1.20:8200/AlbumArt/22-1.jpg"));
    REQUIRE(song.playable() != nullptr);
    CHECK_EQ(song.playable()->duration_us, int64_t{252533000});
    const DlnaObject &film = p.objects[2];
    const DlnaResource *r = film.playable();
    REQUIRE(r != nullptr);
    CHECK_EQ(r->url, std::string("http://192.168.1.20:8200/MediaItems/31.mkv"));  // the larger video, not the thumbnail
    CHECK(r->seekable);  // time seeking (DLNA.ORG_OP=10)
    CHECK_EQ(film.poster_url(), std::string("http://192.168.1.20:8200/Thumbnails/31.jpg"));
    // Escaped (not CDATA) is the same.
    DlnaPage q;
    REQUIRE(parse_dlna_browse_response(envelope(didl, 3, 3, false), "http://192.168.1.20:8200/ctl/ContentDir", &q, &err));
    CHECK_EQ(q.objects.size(), size_t{3});
}

TEST("dlna: faults and broken answers say why") {
    DlnaPage p;
    std::string err;
    CHECK(!parse_dlna_browse_response(kFault, "http://h/", &p, &err));
    CHECK_EQ(err, std::string("UPnP error 701: No such object"));
    CHECK(!parse_dlna_browse_response("<html>nope</html>", "http://h/", &p, &err));
    CHECK(err.find("not a SOAP envelope") != std::string::npos);
    CHECK(!parse_dlna_browse_response("<s:Envelope", "http://h/", &p, &err));
    CHECK(err.find("SOAP") != std::string::npos);
    CHECK(!parse_dlna_browse_response(envelope("<notdidl/>", 0, 0), "http://h/", &p, &err));
    CHECK(err.find("not DIDL-Lite") != std::string::npos);
    // An empty Result is an empty folder.
    REQUIRE(parse_dlna_browse_response(envelope("", 0, 0), "http://h/", &p, &err));
    CHECK(p.objects.empty());
    // External entities are never resolved.
    const std::string xxe = "<!DOCTYPE d [<!ENTITY x SYSTEM \"file:///etc/passwd\">]><DIDL-Lite><item id=\"1\"><dc:title>&x;</dc:title></item></DIDL-Lite>";
    std::vector<DlnaObject> objs;
    CHECK(!parse_didl_lite(xxe, "http://h/", &objs, &err));
}

TEST("dlna: a ContentDirectory request is a SOAP POST with its action") {
    ScriptedTransport t;
    t.add(200, envelope(didl_of(0, 2), 2, 2));
    ContentDirectory cd(t, server());
    DlnaPage p;
    std::string err;
    REQUIRE(cd.browse("64", 0, 2, &p, {}, &err));
    REQUIRE(t.requests.size() == 1);
    const net::HttpRequest &r = t.requests[0];
    CHECK_EQ(r.method, std::string("POST"));
    CHECK_EQ(r.url, server().control_url);
    bool action = false, type = false;
    for (const auto &[k, v] : r.headers) {
        action = action || (k == "SOAPACTION" && v == "\"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\"");
        type = type || (k == "Content-Type" && v.rfind("text/xml", 0) == 0);
    }
    CHECK(action);
    CHECK(type);
    CHECK_EQ(between(r.body, "<ObjectID>", "</ObjectID>"), std::string("64"));
    REQUIRE(p.objects.size() == 2);
    CHECK_EQ(p.objects[1].playable()->url, std::string("http://192.168.1.20:8200/MediaItems/1.mp4"));  // resolved
}

TEST("dlna: browse_all pages to TotalMatches, or to a short page when the total is unknown") {
    {
        ScriptedTransport t;
        t.add(200, envelope(didl_of(0, 2), 2, 5));
        t.add(200, envelope(didl_of(2, 2), 2, 5));
        t.add(200, envelope(didl_of(4, 1), 1, 5));
        ContentDirectory cd(t, server());
        std::vector<DlnaObject> all;
        std::string err;
        REQUIRE(cd.browse_all("64", &all, {}, &err, 2));
        CHECK_EQ(all.size(), size_t{5});
        CHECK_EQ(all[4].id, std::string("i4"));
        CHECK_EQ(t.requests.size(), size_t{3});
        CHECK_EQ(between(t.requests[2].body, "<StartingIndex>", "</StartingIndex>"), std::string("4"));
    }
    {
        // A server that pages by its own size (fewer than asked) but knows the total.
        ScriptedTransport t;
        t.add(200, envelope(didl_of(0, 1), 1, 3));
        t.add(200, envelope(didl_of(1, 1), 1, 3));
        t.add(200, envelope(didl_of(2, 1), 1, 3));
        ContentDirectory cd(t, server());
        std::vector<DlnaObject> all;
        REQUIRE(cd.browse_all("64", &all, {}, nullptr, 10));
        CHECK_EQ(all.size(), size_t{3});
    }
    {
        ScriptedTransport t;  // TotalMatches 0: until a page comes back short
        t.add(200, envelope(didl_of(0, 3), 3, 0));
        t.add(200, envelope(didl_of(3, 1), 1, 0));
        ContentDirectory cd(t, server());
        std::vector<DlnaObject> all;
        REQUIRE(cd.browse_all("64", &all, {}, nullptr, 3));
        CHECK_EQ(all.size(), size_t{4});
        CHECK_EQ(t.requests.size(), size_t{2});
    }
    {
        ScriptedTransport t;  // the limit stops a server that never ends
        for (int i = 0; i < 10; ++i) t.add(200, envelope(didl_of(i * 2, 2), 2, 0));
        ContentDirectory cd(t, server());
        std::vector<DlnaObject> all;
        REQUIRE(cd.browse_all("64", &all, {}, nullptr, 2, 5));
        CHECK_EQ(all.size(), size_t{5});
        CHECK_EQ(between(t.requests.back().body, "<RequestedCount>", "</RequestedCount>"), std::string("1"));
    }
    {
        ScriptedTransport t;  // a later page fails: false, with what came before kept
        t.add(200, envelope(didl_of(0, 2), 2, 4));
        t.add(503, "busy");
        ContentDirectory cd(t, server());
        std::vector<DlnaObject> all;
        std::string err;
        CHECK(!cd.browse_all("64", &all, {}, &err, 2));
        CHECK_EQ(all.size(), size_t{2});
        CHECK_EQ(err, std::string("HTTP 503"));
    }
}

TEST("dlna: a fault over HTTP 500, a transport error and metadata") {
    ScriptedTransport t;
    t.add(500, kFault);
    ContentDirectory cd(t, server());
    DlnaPage p;
    std::string err;
    CHECK(!cd.browse("nope", 0, 10, &p, {}, &err));
    CHECK_EQ(err, std::string("HTTP 500: UPnP error 701: No such object"));
    CHECK(!cd.browse("64", 0, 10, &p, {}, &err));  // nothing scripted: the transport fails
    CHECK(err.find("onnect") != std::string::npos);
    t.add(200, envelope(didl_of(7, 1), 1, 1));
    DlnaObject o;
    REQUIRE(cd.metadata("i7", &o, {}, &err));
    CHECK_EQ(o.title, std::string("Clip 7"));
    CHECK_EQ(between(t.requests.back().body, "<BrowseFlag>", "</BrowseFlag>"), std::string("BrowseMetadata"));
}

TEST("dlna: a media server is a device with a ContentDirectory") {
    net::UpnpDevice d;
    d.friendly_name = "NAS";
    d.udn = "uuid:1";
    d.services.push_back({"urn:schemas-upnp-org:service:ConnectionManager:1", "cm", "http://h/cm", "", ""});
    MediaServer s;
    CHECK(!media_server_from_device(d, "http://h/desc.xml", &s));
    d.services.push_back({"urn:schemas-upnp-org:service:ContentDirectory:2", "cd", "http://h/cd", "", ""});
    REQUIRE(media_server_from_device(d, "http://h/desc.xml", &s));
    CHECK_EQ(s.name, std::string("NAS"));
    CHECK_EQ(s.control_url, std::string("http://h/cd"));
    CHECK_EQ(s.service_type, std::string("urn:schemas-upnp-org:service:ContentDirectory:2"));
    CHECK_EQ(s.location, std::string("http://h/desc.xml"));
}
