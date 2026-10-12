// SPDX-License-Identifier: GPL-3.0-or-later
//
// The home network: finding services with mDNS/DNS-SD (a media server, a game-streaming PC, a
// printer) and waking a computer with a Wake-on-LAN magic packet.
//
// Discovery sends "legacy unicast" queries (RFC 6762 §6.7): from an ordinary UDP port to the mDNS
// group, answered by unicast to that port. The app never binds port 5353, which another process
// (or the TV's own responder) may hold, and needs no multicast group membership. Both calls block:
// run them on a worker. Neither has been tried from a Developer Mode app on a TV yet.
#pragma once

#include "wnk/async/async.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wnk::net {

struct LanService {
    std::string instance;  // full name, "Living Room._googlecast._tcp.local"
    std::string name;      // its first label, "Living Room"
    std::string host;      // "media-pc.local"
    std::string address;   // IPv4, "192.168.1.20" (empty if no A record came)
    uint16_t port = 0;
    std::vector<std::pair<std::string, std::string>> txt;  // key=value TXT entries, in order
    std::string txt_value(const std::string &key) const;
};

// What the responses so far say. SRV, TXT and A records may come in different packets, so addresses
// are kept apart and joined to services by host name in resolve().
struct MdnsAnswers {
    std::vector<LanService> services;
    std::vector<std::pair<std::string, std::string>> addresses;  // lower-case host → IPv4
    void resolve();
};

// A PTR query for `service` ("_http._tcp.local"; ".local" is added if missing).
std::vector<uint8_t> mdns_query(const std::string &service, uint16_t id);
// Adds what one response packet says about `service`. False if it is not a well-formed DNS response.
bool mdns_parse_response(const uint8_t *data, size_t size, const std::string &service, MdnsAnswers *answers);
// Asks the network for `service` and collects answers for `wait` (or until cancelled).
std::vector<LanService> mdns_browse(const std::string &service, std::chrono::milliseconds wait,
                                    const async::CancellationToken &token = {}, std::string *error = nullptr);

// "AA:BB:CC:DD:EE:FF", "aa-bb-cc-dd-ee-ff" or "aabbccddeeff".
bool parse_mac(std::string_view text, std::array<uint8_t, 6> *mac);
// Six 0xFF bytes and sixteen copies of the MAC.
std::vector<uint8_t> wol_packet(const std::array<uint8_t, 6> &mac);
// Sends the magic packet as a UDP broadcast (to `address`, port `port`).
bool wake_on_lan(std::string_view mac, const std::string &address = "255.255.255.255", uint16_t port = 9,
                 std::string *error = nullptr);

}  // namespace wnk::net
