// SPDX-License-Identifier: GPL-3.0-or-later
//
// net/lan: the mDNS query and response codec (name compression, records split over packets), MAC
// parsing and the Wake-on-LAN packet sent to a local UDP socket; system payloads and the connection
// status reply (platform/system).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/net/lan.hpp"
#include "wnk/platform/system.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace wnk;

namespace {

struct Packet {
    std::vector<uint8_t> b;
    void u16(uint16_t x) {
        b.push_back(static_cast<uint8_t>(x >> 8));
        b.push_back(static_cast<uint8_t>(x));
    }
    void u32(uint32_t x) {
        u16(static_cast<uint16_t>(x >> 16));
        u16(static_cast<uint16_t>(x));
    }
    size_t name(const std::vector<std::string> &labels, int pointer = -1) {
        const size_t at = b.size();
        for (const auto &l : labels) {
            b.push_back(static_cast<uint8_t>(l.size()));
            b.insert(b.end(), l.begin(), l.end());
        }
        if (pointer >= 0) {
            u16(static_cast<uint16_t>(0xC000 | pointer));
        } else {
            b.push_back(0);
        }
        return at;
    }
    void header(uint16_t id, uint16_t answers, uint16_t additional) {
        u16(id);
        u16(0x8400);  // response, authoritative
        u16(0);
        u16(answers);
        u16(0);
        u16(additional);
    }
    void record(uint16_t type, const std::vector<uint8_t> &rdata) {
        u16(type);
        u16(1);
        u32(120);
        u16(static_cast<uint16_t>(rdata.size()));
        b.insert(b.end(), rdata.begin(), rdata.end());
    }
};

}  // namespace

TEST("mdns: the query asks for the service's PTR records") {
    const auto q = net::mdns_query("_http._tcp", 0x1234);
    CHECK_EQ(q[0], uint8_t{0x12});
    CHECK_EQ(q[1], uint8_t{0x34});
    const std::string expect = "\x05_http\x04_tcp\x05local";
    CHECK(std::memcmp(q.data() + 12, expect.data(), expect.size()) == 0);
    CHECK_EQ(q[12 + expect.size()], uint8_t{0});
    CHECK_EQ(q[12 + expect.size() + 2], uint8_t{12});  // type PTR
}

TEST("mdns: PTR, SRV, TXT and A records with compressed names make a service") {
    Packet p;
    p.header(7, 1, 3);
    const size_t service = p.name({"_http", "_tcp", "local"});
    p.u16(12);  // PTR
    p.u16(1);
    p.u32(120);
    const size_t len_at = p.b.size();
    p.u16(0);
    const size_t instance = p.name({"Media PC"}, static_cast<int>(service));
    p.b[len_at + 1] = static_cast<uint8_t>(p.b.size() - instance);
    // SRV: priority, weight, port 8096, target media-pc.local
    p.name({}, static_cast<int>(instance));
    std::vector<uint8_t> srv = {0, 0, 0, 0, 0x1F, 0xA0, 8, 'm', 'e', 'd', 'i', 'a', '-', 'p', 'c', 5, 'l', 'o', 'c', 'a', 'l', 0};
    p.record(33, srv);
    p.name({}, static_cast<int>(instance));
    p.record(16, {9, 'v', 'e', 'r', 's', 'i', 'o', 'n', '=', '2', 4, 't', 'l', 's', '=', 0});
    p.name({"media-pc", "local"});
    p.record(1, {192, 168, 1, 20});
    net::MdnsAnswers answers;
    REQUIRE(net::mdns_parse_response(p.b.data(), p.b.size(), "_http._tcp.local", &answers));
    REQUIRE(answers.services.size() == 1u);
    const auto &s = answers.services[0];
    CHECK_EQ(s.instance, std::string("Media PC._http._tcp.local"));
    CHECK_EQ(s.name, std::string("Media PC"));
    CHECK_EQ(s.host, std::string("media-pc.local"));
    CHECK_EQ(s.port, uint16_t{8096});
    CHECK_EQ(s.address, std::string("192.168.1.20"));
    CHECK_EQ(s.txt_value("VERSION"), std::string("2"));
    CHECK_EQ(s.txt.size(), size_t{2});
}

TEST("mdns: an address that arrives first is joined later; garbage and loops are refused") {
    net::MdnsAnswers answers;
    Packet a;
    a.header(1, 1, 0);
    a.name({"box", "local"});
    a.record(1, {10, 0, 0, 5});
    REQUIRE(net::mdns_parse_response(a.b.data(), a.b.size(), "_x._tcp", &answers));
    CHECK(answers.services.empty());
    Packet s;
    s.header(1, 1, 0);
    s.name({"Box", "_x", "_tcp", "local"});
    s.record(33, {0, 0, 0, 0, 0, 80, 3, 'b', 'o', 'x', 5, 'l', 'o', 'c', 'a', 'l', 0});
    REQUIRE(net::mdns_parse_response(s.b.data(), s.b.size(), "_x._tcp", &answers));
    REQUIRE(answers.services.size() == 1u);
    CHECK_EQ(answers.services[0].address, std::string("10.0.0.5"));
    const uint8_t query_not_response[12] = {0, 1, 0, 0, 0, 1};
    CHECK(!net::mdns_parse_response(query_not_response, sizeof query_not_response, "_x._tcp", &answers));
    Packet loop;
    loop.header(1, 1, 0);
    loop.u16(0xC00C);  // a name pointing at itself
    loop.record(1, {1, 2, 3, 4});
    CHECK(!net::mdns_parse_response(loop.b.data(), loop.b.size(), "_x._tcp", &answers));
    Packet truncated;
    truncated.header(1, 1, 0);
    truncated.name({"box", "local"});
    truncated.u16(1);
    CHECK(!net::mdns_parse_response(truncated.b.data(), truncated.b.size(), "_x._tcp", &answers));
}

TEST("mdns: browsing the host's own responder for service types (skipped without one)") {
    std::string err;
    const auto found = net::mdns_browse("_services._dns-sd._udp.local", std::chrono::milliseconds(700), {}, &err);
    if (!err.empty() || found.empty()) {
        std::printf("  SKIP (no mDNS answer on this host: %s)\n", err.empty() ? "nothing answered" : err.c_str());
        return;
    }
    for (const auto &s : found) std::printf("  found %s\n", s.instance.c_str());
    CHECK(!found[0].name.empty());
}

TEST("wake-on-lan: MAC parsing and the magic packet reach a UDP socket") {
    std::array<uint8_t, 6> mac{};
    CHECK(net::parse_mac("AA:bb:CC:01:02:03", &mac));
    CHECK_EQ(mac[0], uint8_t{0xAA});
    CHECK_EQ(mac[5], uint8_t{3});
    CHECK(net::parse_mac("aa-bb-cc-01-02-03", &mac));
    CHECK(net::parse_mac("aabbcc010203", &mac));
    CHECK(!net::parse_mac("aa:bb:cc:01:02", &mac));
    CHECK(!net::parse_mac("zz:bb:cc:01:02:03", &mac));
    const auto packet = net::wol_packet(mac);
    CHECK_EQ(packet.size(), size_t{102});
    CHECK_EQ(packet[5], uint8_t{0xFF});
    CHECK_EQ(packet[6 + 16 * 6 - 1], uint8_t{3});

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0);
    socklen_t len = sizeof addr;
    getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
    std::string err;
    CHECK(net::wake_on_lan("aa:bb:cc:01:02:03", "127.0.0.1", ntohs(addr.sin_port), &err));
    pollfd p{fd, POLLIN, 0};
    REQUIRE(poll(&p, 1, 2000) == 1);
    uint8_t buf[200];
    const ssize_t n = recv(fd, buf, sizeof buf, 0);
    CHECK_EQ(n, ssize_t{102});
    CHECK(std::memcmp(buf, packet.data(), packet.size()) == 0);
    close(fd);
    CHECK(!net::wake_on_lan("nope", "127.0.0.1", 9, &err));
    CHECK(!net::wake_on_lan("aa:bb:cc:01:02:03", "not-an-address", 9, &err));
}

TEST("system: launch, browser and toast payloads; replies; connection status") {
    CHECK_EQ(platform::launch_payload("com.webos.app.home"), std::string(R"({"id":"com.webos.app.home"})"));
    CHECK_EQ(platform::launch_payload("org.x", R"({"q":"a \"b\""})"), std::string(R"({"id":"org.x","params":{"q":"a \"b\""}})"));
    CHECK_EQ(platform::launch_payload("org.x", "[1]"), std::string(R"({"id":"org.x"})"));  // params must be an object
    CHECK_EQ(platform::open_url_payload("https://example.org/?a=1"),
             std::string(R"({"id":"com.webos.app.browser","params":{"target":"https://example.org/?a=1"}})"));
    CHECK_EQ(platform::toast_payload("org.x", "Saved\n"), std::string(R"({"message":"Saved\n","noaction":true,"sourceId":"org.x"})"));
    std::string why;
    CHECK(platform::reply_ok(R"({"returnValue":true})"));
    CHECK(!platform::reply_ok(R"({"returnValue":false,"errorText":"Denied"})", &why));
    CHECK_EQ(why, std::string("Denied"));
    CHECK(!platform::reply_ok("x", &why));

    platform::NetworkStatus s;
    CHECK(platform::parse_connection_status(
        R"({"returnValue":true,"isInternetConnectionAvailable":true,"wired":{"state":"connected"},"wifi":{"state":"disconnected"}})", &s));
    CHECK(s.known && s.internet && s.wired && !s.wifi);
    CHECK_EQ(s.summary(), std::string("online (wired)"));
    CHECK(platform::parse_connection_status(R"({"isInternetConnectionAvailable":false,"wifi":{"state":"connected"}})", &s));
    CHECK_EQ(s.summary(), std::string("offline (Wi-Fi, no internet)"));
    CHECK(platform::parse_connection_status(R"({"isInternetConnectionAvailable":false,"wired":{"state":"disconnected"}})", &s));
    CHECK_EQ(s.summary(), std::string("offline"));
    CHECK(!platform::parse_connection_status(R"({"returnValue":false,"errorText":"x"})", &s));
    CHECK(!platform::parse_connection_status(R"({"subscribed":true})", &s));
    CHECK_EQ(platform::NetworkStatus{}.summary(), std::string("unknown"));
    bool on = false;
    CHECK(platform::parse_audio_guidance(R"({"returnValue":true,"settings":{"audioGuidance":"on"},"category":"option"})", &on));
    CHECK(on);
    CHECK(platform::parse_audio_guidance(R"({"settings":{"audioGuidance":"off"}})", &on));
    CHECK(!on);
    CHECK(!platform::parse_audio_guidance(R"({"settings":{}})", &on));
    CHECK_EQ(platform::speak_payload("Play, button", "en-GB"), std::string(R"({"clear":true,"language":"en-GB","text":"Play, button"})"));
    CHECK(platform::launch_app("x").status == platform::LunaStatus::Unsupported);  // host: no bus
}
