// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/net/lan.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <random>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wnk::net {
namespace {

constexpr uint16_t kTypeA = 1, kTypePtr = 12, kTypeTxt = 16, kTypeSrv = 33;

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string with_local(std::string service) {
    while (!service.empty() && service.back() == '.') service.pop_back();
    if (lower(service).size() < 6 || lower(service).compare(service.size() - 6, 6, ".local") != 0) service += ".local";
    return service;
}

void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

uint16_t get16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// A (possibly compressed) name at `pos`; advances `pos` past it in the record. False if malformed.
bool read_name(const uint8_t *data, size_t size, size_t &pos, std::string *out) {
    std::string name;
    size_t p = pos;
    bool jumped = false;
    for (int hops = 0; hops < 64; ++hops) {
        if (p >= size) return false;
        const uint8_t len = data[p];
        if ((len & 0xC0) == 0xC0) {
            if (p + 1 >= size) return false;
            const size_t target = static_cast<size_t>((len & 0x3F) << 8 | data[p + 1]);
            if (!jumped) pos = p + 2;
            jumped = true;
            p = target;
            continue;
        }
        if (len == 0) {
            if (!jumped) pos = p + 1;
            *out = name;
            return true;
        }
        if ((len & 0xC0) != 0 || p + 1 + len > size) return false;
        if (!name.empty()) name += '.';
        name.append(reinterpret_cast<const char *>(data + p + 1), len);
        p += 1 + len;
    }
    return false;  // a pointer loop
}

LanService &service_for(std::vector<LanService> *services, const std::string &instance) {
    for (auto &s : *services) {
        if (lower(s.instance) == lower(instance)) return s;
    }
    LanService s;
    s.instance = instance;
    services->push_back(std::move(s));
    return services->back();
}

}  // namespace

std::string LanService::txt_value(const std::string &key) const {
    for (const auto &[k, v] : txt) {
        if (lower(k) == lower(key)) return v;
    }
    return {};
}

std::vector<uint8_t> mdns_query(const std::string &service, uint16_t id) {
    std::vector<uint8_t> q;
    put16(q, id);
    put16(q, 0);  // a standard query
    put16(q, 1);  // one question
    put16(q, 0);
    put16(q, 0);
    put16(q, 0);
    const std::string name = with_local(service);
    size_t start = 0;
    while (start < name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        const size_t len = std::min<size_t>(dot - start, 63);
        q.push_back(static_cast<uint8_t>(len));
        q.insert(q.end(), name.begin() + static_cast<long>(start), name.begin() + static_cast<long>(start + len));
        start = dot + 1;
    }
    q.push_back(0);
    put16(q, kTypePtr);
    put16(q, 1);  // class IN
    return q;
}

void MdnsAnswers::resolve() {
    for (auto &s : services) {
        for (const auto &[host, addr] : addresses) {
            if (lower(s.host) == host) s.address = addr;
        }
    }
}

bool mdns_parse_response(const uint8_t *data, size_t size, const std::string &service, MdnsAnswers *answers) {
    std::vector<LanService> *services = &answers->services;
    if (data == nullptr || size < 12 || (data[2] & 0x80) == 0) return false;  // not a response
    const uint16_t qd = get16(data + 4);
    const size_t records = static_cast<size_t>(get16(data + 6)) + get16(data + 8) + get16(data + 10);
    size_t pos = 12;
    for (uint16_t i = 0; i < qd; ++i) {
        std::string ignored;
        if (!read_name(data, size, pos, &ignored) || pos + 4 > size) return false;
        pos += 4;
    }
    const std::string want = lower(with_local(service));
    for (size_t r = 0; r < records; ++r) {
        std::string owner;
        if (!read_name(data, size, pos, &owner) || pos + 10 > size) return false;
        const uint16_t type = get16(data + pos);
        const uint16_t len = get16(data + pos + 8);
        pos += 10;
        if (pos + len > size) return false;
        const size_t end = pos + len;
        if (type == kTypePtr && lower(owner) == want) {
            size_t p = pos;
            std::string instance;
            if (read_name(data, size, p, &instance)) {
                LanService &s = service_for(services, instance);
                const size_t cut = instance.size() > want.size() ? instance.size() - want.size() - 1 : std::string::npos;
                s.name = cut != std::string::npos ? instance.substr(0, cut) : instance;
            }
        } else if (type == kTypeSrv && len >= 7) {
            size_t p = pos + 6;
            std::string target;
            if (read_name(data, size, p, &target) && lower(owner).find(want) != std::string::npos) {
                LanService &s = service_for(services, owner);
                s.port = get16(data + pos + 4);
                s.host = target;
            }
        } else if (type == kTypeTxt && lower(owner).find(want) != std::string::npos) {
            LanService &s = service_for(services, owner);
            s.txt.clear();
            for (size_t p = pos; p < end;) {
                const uint8_t n = data[p];
                if (p + 1 + n > end) break;
                const std::string entry(reinterpret_cast<const char *>(data + p + 1), n);
                if (!entry.empty()) {
                    const size_t eq = entry.find('=');
                    s.txt.emplace_back(entry.substr(0, eq), eq == std::string::npos ? "" : entry.substr(eq + 1));
                }
                p += 1 + n;
            }
        } else if (type == kTypeA && len == 4) {
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, data + pos, buf, sizeof buf);
            const std::string host = lower(owner);
            auto known = std::find_if(answers->addresses.begin(), answers->addresses.end(), [&](const auto &a) { return a.first == host; });
            if (known != answers->addresses.end()) {
                known->second = buf;
            } else {
                answers->addresses.emplace_back(host, buf);
            }
        }
        pos = end;
    }
    answers->resolve();
    return true;
}

std::vector<LanService> mdns_browse(const std::string &service, std::chrono::milliseconds wait,
                                    const async::CancellationToken &token, std::string *error) {
    MdnsAnswers found;
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error != nullptr) *error = std::string("socket: ") + std::strerror(errno);
        return {};
    }
    const unsigned char ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);
    std::random_device rd;
    const auto id = static_cast<uint16_t>(rd());
    const auto query = mdns_query(service, id);
    const auto deadline = std::chrono::steady_clock::now() + wait;
    // Ask twice: one lost datagram should not hide a device.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (sendto(fd, query.data(), query.size(), 0, reinterpret_cast<sockaddr *>(&group), sizeof group) < 0 && attempt == 0) {
            if (error != nullptr) *error = std::string("sendto 224.0.0.251:5353: ") + std::strerror(errno);
            close(fd);
            return {};
        }
        const auto until = attempt == 0 ? std::min(deadline, std::chrono::steady_clock::now() + wait / 3) : deadline;
        while (!token.cancelled()) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
            if (left.count() <= 0) break;
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, static_cast<int>(std::min<long long>(left.count(), 100))) <= 0) continue;
            uint8_t buf[9000];
            const ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n > 12 && get16(buf) == id) mdns_parse_response(buf, static_cast<size_t>(n), service, &found);
        }
    }
    close(fd);
    found.resolve();
    return found.services;
}

bool parse_mac(std::string_view text, std::array<uint8_t, 6> *mac) {
    std::string hex;
    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            hex += c;
        } else if (c != ':' && c != '-' && c != '.') {
            return false;
        }
    }
    if (hex.size() != 12) return false;
    for (size_t i = 0; i < 6; ++i) (*mac)[i] = static_cast<uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    return true;
}

std::vector<uint8_t> wol_packet(const std::array<uint8_t, 6> &mac) {
    std::vector<uint8_t> p(6, 0xFF);
    for (int i = 0; i < 16; ++i) p.insert(p.end(), mac.begin(), mac.end());
    return p;
}

bool wake_on_lan(std::string_view mac_text, const std::string &address, uint16_t port, std::string *error) {
    std::array<uint8_t, 6> mac{};
    if (!parse_mac(mac_text, &mac)) {
        if (error != nullptr) *error = "not a MAC address";
        return false;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &to.sin_addr) != 1) {
        if (error != nullptr) *error = "not an IPv4 address: " + address;
        return false;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error != nullptr) *error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    const auto packet = wol_packet(mac);
    const bool ok = sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof to) ==
                    static_cast<ssize_t>(packet.size());
    if (!ok && error != nullptr) *error = std::string("sendto: ") + std::strerror(errno);
    close(fd);
    return ok;
}

}  // namespace wnk::net
