// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/net/ssdp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace wnk::net {
namespace {

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, z = s.size();
    while (a < z && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (z > a && std::isspace(static_cast<unsigned char>(s[z - 1]))) --z;
    return s.substr(a, z - a);
}

// The five predefined entities and numeric references (descriptions are XML).
std::string unescape(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        const std::string e = s.substr(i + 1, semi - i - 1);
        unsigned long cp = 0;
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (e.size() > 1 && e[0] == '#') {
            cp = e[1] == 'x' || e[1] == 'X' ? std::strtoul(e.c_str() + 2, nullptr, 16) : std::strtoul(e.c_str() + 1, nullptr, 10);
            if (cp == 0 || cp > 0x10FFFF) {
                out += s.substr(i, semi - i + 1);
            } else if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6)), out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12)), out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18)), out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        } else {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

// The element `name` (any namespace prefix) found from `from` within [from, end): its content, and
// where it ends. Descriptions are shallow and regular, so a scan is enough; CDATA is unwrapped.
bool element(const std::string &xml, const std::string &name, size_t from, size_t end, std::string *content,
             size_t *after) {
    size_t pos = from;
    while (pos < end) {
        const size_t lt = xml.find('<', pos);
        if (lt == std::string::npos || lt >= end) return false;
        size_t n = lt + 1;
        if (n < end && (xml[n] == '/' || xml[n] == '?' || xml[n] == '!')) {
            pos = n;
            continue;
        }
        size_t name_end = n;
        while (name_end < end && !std::isspace(static_cast<unsigned char>(xml[name_end])) && xml[name_end] != '>' &&
               xml[name_end] != '/') {
            ++name_end;
        }
        std::string tag = xml.substr(n, name_end - n);
        if (const size_t colon = tag.find(':'); colon != std::string::npos) tag = tag.substr(colon + 1);
        const size_t gt = xml.find('>', name_end);
        if (gt == std::string::npos || gt >= end) return false;
        if (tag != name) {
            pos = gt + 1;
            continue;
        }
        if (xml[gt - 1] == '/') {  // <name/>
            content->clear();
            *after = gt + 1;
            return true;
        }
        // The matching close tag at the same depth (an element may contain one of its own name,
        // as a device holds embedded devices).
        int depth = 1;
        size_t scan = gt + 1;
        while (scan < end) {
            const size_t next = xml.find('<', scan);
            if (next == std::string::npos || next >= end) return false;
            const bool closing = next + 1 < end && xml[next + 1] == '/';
            size_t t = next + (closing ? 2 : 1), te = t;
            while (te < end && !std::isspace(static_cast<unsigned char>(xml[te])) && xml[te] != '>' && xml[te] != '/') ++te;
            std::string other = xml.substr(t, te - t);
            if (const size_t colon = other.find(':'); colon != std::string::npos) other = other.substr(colon + 1);
            const size_t close_gt = xml.find('>', te);
            if (close_gt == std::string::npos || close_gt >= end) return false;
            if (other == name && xml[close_gt - 1] != '/') depth += closing ? -1 : 1;
            if (depth == 0) {
                std::string c = xml.substr(gt + 1, next - gt - 1);
                const std::string cdata = "<![CDATA[";
                if (c.compare(0, cdata.size(), cdata) == 0 && c.size() >= cdata.size() + 3) {
                    c = c.substr(cdata.size(), c.size() - cdata.size() - 3);
                }
                *content = c;
                *after = close_gt + 1;
                return true;
            }
            scan = close_gt + 1;
        }
        return false;
    }
    return false;
}

std::string text_of(const std::string &xml, const std::string &name) {
    std::string c;
    size_t after = 0;
    return element(xml, name, 0, xml.size(), &c, &after) ? trim(unescape(c)) : std::string();
}

}  // namespace

std::string SsdpResponse::header(const std::string &name) const {
    const std::string want = lower(name);
    for (const auto &[k, v] : headers) {
        if (k == want) return v;
    }
    return {};
}

std::string SsdpResponse::uuid() const {
    const size_t sep = usn.find("::");
    return sep == std::string::npos ? usn : usn.substr(0, sep);
}

std::string ssdp_search_request(const std::string &st, const SsdpOptions &o) {
    return "M-SEARCH * HTTP/1.1\r\n"
           "HOST: " + o.address + ":" + std::to_string(o.port) + "\r\n"
           "MAN: \"ssdp:discover\"\r\n"
           "MX: " + std::to_string(std::clamp(o.mx, 1, 5)) + "\r\n"
           "ST: " + st + "\r\n"
           "USER-AGENT: webOS/1 UPnP/2.0 wnk/1\r\n\r\n";
}

bool ssdp_parse_response(const char *data, size_t size, SsdpResponse *out) {
    const std::string text(data, size);
    const size_t eol = text.find('\n');
    if (eol == std::string::npos) return false;
    std::string status = text.substr(0, eol);
    if (!status.empty() && status.back() == '\r') status.pop_back();
    // "HTTP/1.1 200 OK" (some devices omit the reason phrase)
    if (status.compare(0, 5, "HTTP/") != 0 || status.find(" 200") == std::string::npos) return false;
    *out = SsdpResponse{};
    size_t pos = eol + 1;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = end + 1;
        if (line.empty()) break;
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        out->headers.emplace_back(lower(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
    }
    out->location = out->header("location");
    out->st = out->header("st");
    out->usn = out->header("usn");
    out->server = out->header("server");
    const std::string cache = lower(out->header("cache-control"));
    if (const size_t at = cache.find("max-age"); at != std::string::npos) {
        const size_t eq = cache.find('=', at);
        if (eq != std::string::npos) out->max_age_s = std::atoi(cache.c_str() + eq + 1);
    }
    return !out->location.empty();
}

std::vector<SsdpResponse> ssdp_search(const std::string &st, std::chrono::milliseconds wait,
                                      const async::CancellationToken &token, std::string *error, const SsdpOptions &o) {
    std::vector<SsdpResponse> found;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(o.port);
    if (inet_pton(AF_INET, o.address.c_str(), &to.sin_addr) != 1) {
        if (error != nullptr) *error = "not an IPv4 address: " + o.address;
        return found;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error != nullptr) *error = std::string("socket: ") + std::strerror(errno);
        return found;
    }
    const unsigned char ttl = 2;  // UDA: "SHOULD default to 2"
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    const std::string request = ssdp_search_request(st, o);
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (sendto(fd, request.data(), request.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof to) < 0 && attempt == 0) {
            if (error != nullptr) *error = "sendto " + o.address + ":" + std::to_string(o.port) + ": " + std::strerror(errno);
            close(fd);
            return found;
        }
        const auto until = attempt == 0 ? std::min(deadline, std::chrono::steady_clock::now() + wait / 3) : deadline;
        while (!token.cancelled()) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
            if (left.count() <= 0) break;
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, static_cast<int>(std::min<long long>(left.count(), 100))) <= 0) continue;
            char buf[4096];
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const ssize_t n = recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr *>(&from), &len);
            SsdpResponse r;
            if (n <= 0 || !ssdp_parse_response(buf, static_cast<size_t>(n), &r)) continue;
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            r.address = ip;
            const std::string key = r.usn.empty() ? r.location + "|" + r.st : r.usn;
            const bool seen = std::any_of(found.begin(), found.end(), [&](const SsdpResponse &f) {
                return (f.usn.empty() ? f.location + "|" + f.st : f.usn) == key;
            });
            if (!seen) found.push_back(std::move(r));
        }
    }
    close(fd);
    return found;
}

std::string resolve_http_url(const std::string &base, const std::string &ref) {
    if (ref.empty()) return {};
    if (ref.find("://") != std::string::npos) return ref;
    const size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) return ref;
    if (ref.compare(0, 2, "//") == 0) return base.substr(0, scheme_end + 1) + ref;
    const size_t path_start = base.find('/', scheme_end + 3);
    const std::string origin = path_start == std::string::npos ? base : base.substr(0, path_start);
    if (ref[0] == '/') return origin + ref;
    std::string dir = path_start == std::string::npos ? "/" : base.substr(path_start);
    dir = dir.substr(0, dir.find_first_of("?#"));
    dir = dir.substr(0, dir.rfind('/') + 1);
    // Remove "./" and "../" segments.
    std::string path = dir + ref, out;
    std::vector<std::string> parts;
    size_t from = 0;
    const size_t q = path.find_first_of("?#");
    const std::string tail = q == std::string::npos ? "" : path.substr(q);
    if (q != std::string::npos) path.resize(q);
    while (from <= path.size()) {
        size_t slash = path.find('/', from);
        if (slash == std::string::npos) slash = path.size();
        const std::string seg = path.substr(from, slash - from);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (seg != "." && !(seg.empty() && slash != path.size())) {
            parts.push_back(seg);
        }
        from = slash + 1;
    }
    for (const std::string &p : parts) out += "/" + p;
    if (path.size() > 1 && path.back() == '/' && (out.empty() || out.back() != '/')) out += "/";
    return origin + (out.empty() ? "/" : out) + tail;
}

bool parse_upnp_description(const std::string &xml, const std::string &location, UpnpDevice *out, std::string *error) {
    *out = UpnpDevice{};
    std::string device;
    size_t after = 0;
    if (!element(xml, "device", 0, xml.size(), &device, &after)) {
        if (error != nullptr) *error = "no <device> in the description";
        return false;
    }
    const std::string base_text = text_of(xml, "URLBase");
    const std::string base = base_text.empty() ? location : base_text;
    // The root's own fields come before any embedded <deviceList>: read them from that part.
    std::string own = device;
    if (const size_t list = own.find("deviceList>"); list != std::string::npos) {
        const size_t lt = own.rfind('<', list);
        own = own.substr(0, lt == std::string::npos ? list : lt);
    }
    out->device_type = text_of(own, "deviceType");
    out->friendly_name = text_of(own, "friendlyName");
    out->manufacturer = text_of(own, "manufacturer");
    out->model_name = text_of(own, "modelName");
    out->model_number = text_of(own, "modelNumber");
    out->serial_number = text_of(own, "serialNumber");
    out->udn = text_of(own, "UDN");
    const std::string pres = text_of(own, "presentationURL");
    out->presentation_url = pres.empty() ? std::string() : resolve_http_url(base, pres);
    // Every <service>, the embedded devices' too.
    size_t pos = 0;
    std::string svc;
    while (element(device, "service", pos, device.size(), &svc, &after)) {
        UpnpService s;
        s.type = text_of(svc, "serviceType");
        s.id = text_of(svc, "serviceId");
        s.control_url = resolve_http_url(base, text_of(svc, "controlURL"));
        s.event_url = resolve_http_url(base, text_of(svc, "eventSubURL"));
        s.scpd_url = resolve_http_url(base, text_of(svc, "SCPDURL"));
        if (!s.type.empty()) out->services.push_back(std::move(s));
        pos = after;
    }
    if (out->udn.empty() && out->friendly_name.empty()) {
        if (error != nullptr) *error = "the description names no device";
        return false;
    }
    return true;
}

bool fetch_upnp_description(HttpTransport &http, const std::string &location, UpnpDevice *out,
                            const async::CancellationToken &token, std::string *error) {
    HttpRequest req;
    req.url = location;
    req.timeout_ms = 5000;
    req.connect_timeout_ms = 3000;
    req.max_response_bytes = 256u << 10;
    const HttpResponse r = http.perform(req, token);
    if (!r.ok()) {
        if (error != nullptr) {
            *error = r.error != NetError::None ? std::string(error_name(r.error)) + (r.detail.empty() ? "" : ": " + r.detail)
                                               : "HTTP " + std::to_string(r.status);
        }
        return false;
    }
    return parse_upnp_description(r.body, r.effective_url.empty() ? location : r.effective_url, out, error);
}

}  // namespace wnk::net
