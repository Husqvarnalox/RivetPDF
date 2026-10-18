// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "net/Ip.hpp"

#include <cstdio>
#include <string>

using namespace rivet::net;

namespace {

std::optional<IpAddress> parseAny(const std::string& s) {
    return s.find(':') != std::string::npos ? parseIpv6(s) : parseIpv4Strict(s);
}

void expectForbidden(const char* text, bool forbidden) {
    const auto ip = parseAny(text);
    if (!ip) std::fprintf(stderr, "    unparsable address in table: %s\n", text);
    CHECK(ip.has_value());
    if (isForbiddenAddress(*ip) != forbidden) std::fprintf(stderr, "    %s: expected forbidden=%d\n", text, forbidden);
    CHECK(isForbiddenAddress(*ip) == forbidden);
}

} // namespace

RIVET_TEST(netForbiddenIpv4) {
    for (const char* a : {"0.0.0.0", "0.255.255.255", "10.0.0.1", "10.255.255.255", "127.0.0.1", "127.255.255.254",
                          "172.16.0.1", "172.31.255.255", "192.168.0.1", "192.168.255.255", "169.254.169.254",
                          "169.254.0.1", "100.64.0.1", "100.127.255.255", "224.0.0.1", "239.255.255.255",
                          "240.0.0.1", "255.255.255.255", "192.0.0.1", "192.0.2.1", "198.18.0.1", "198.19.255.255",
                          "198.51.100.7", "203.0.113.9", "192.88.99.1"})
        expectForbidden(a, true);
}

RIVET_TEST(netAllowedIpv4) {
    for (const char* a : {"8.8.8.8", "1.1.1.1", "9.255.255.255", "11.0.0.1", "100.63.255.255", "100.128.0.1",
                          "126.255.255.255", "128.0.0.1", "172.15.255.255", "172.32.0.1", "192.167.255.255",
                          "192.169.0.1", "169.253.255.255", "169.255.0.1", "198.17.255.255", "198.20.0.1",
                          "223.255.255.255", "93.184.216.34"})
        expectForbidden(a, false);
}

RIVET_TEST(netForbiddenIpv6) {
    for (const char* a : {"::", "::1", "fe80::1", "febf::1", "fc00::1", "fd12:3456::1", "fdff::1", "ff02::1", "ff00::",
                          "fec0::1", "2001:db8::1", "2001:0::1", "100::1",
                          // IPv4-mapped / compatible: embedded address classified.
                          "::ffff:127.0.0.1", "::ffff:10.1.2.3", "::ffff:169.254.169.254", "::ffff:192.168.1.1",
                          "::ffff:0.0.0.0", "::127.0.0.1", "::10.0.0.1",
                          // NAT64
                          "64:ff9b::127.0.0.1", "64:ff9b::a00:1", "64:ff9b::169.254.169.254", "64:ff9b:1::1",
                          // 6to4 (embedded v4 is bytes 2..5)
                          "2002:7f00:1::1", "2002:0a00:0001::", "2002:a9fe:a9fe::1", "2002:c0a8:0101::"})
        expectForbidden(a, true);
}

RIVET_TEST(netAllowedIpv6) {
    for (const char* a : {"2001:4860:4860::8888", "2606:4700:4700::1111", "::ffff:8.8.8.8", "64:ff9b::8.8.8.8",
                          "2002:0808:0808::1", "2a00:1450:4001::200e", "fbff::1", "fe00::1", "2000::1"})
        expectForbidden(a, false);
}

RIVET_TEST(netIpv4LegacyForms) {
    struct Row { const char* in; const char* out; };
    const Row ok[] = {{"2130706433", "127.0.0.1"}, {"0x7f.1", "127.0.0.1"}, {"0177.0.0.1", "127.0.0.1"},
                      {"127.1", "127.0.0.1"},     {"0x7f000001", "127.0.0.1"}, {"127.0.1", "127.0.0.1"},
                      {"0", "0.0.0.0"},           {"4294967295", "255.255.255.255"}, {"10.1.1", "10.1.0.1"},
                      {"0X7F.0X0.0.1", "127.0.0.1"}};
    for (const auto& row : ok) {
        const auto ip = parseIpv4Legacy(row.in);
        if (!ip) std::fprintf(stderr, "    legacy parse failed: %s\n", row.in);
        CHECK(ip.has_value());
        CHECK_EQ(formatIp(*ip), std::string(row.out));
    }
    for (const char* bad : {"", "4294967296", "256.1.1.1", "1.2.3.4.5", "1..2", "08", "0xg", "0x", "1.16777216",
                            "-1", "1.2.3.", ".1", "1 2"})
        CHECK(!parseIpv4Legacy(bad).has_value());
}

RIVET_TEST(netIpv6ParseFormat) {
    struct Row { const char* in; const char* out; };
    const Row ok[] = {{"::1", "::1"}, {"::", "::"}, {"2001:DB8:0:0:0:0:0:1", "2001:db8::1"},
                      {"::ffff:1.2.3.4", "::ffff:102:304"}, {"1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"},
                      {"1::", "1::"}, {"fe80::1", "fe80::1"}, {"1:0:0:2:0:0:0:3", "1:0:0:2::3"}};
    for (const auto& row : ok) {
        const auto ip = parseIpv6(row.in);
        if (!ip) std::fprintf(stderr, "    v6 parse failed: %s\n", row.in);
        CHECK(ip.has_value());
        CHECK_EQ(formatIp(*ip), std::string(row.out));
    }
    for (const char* bad : {"", ":", ":::", "1::2::3", "12345::1", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9", "g::1",
                            "::1%eth0", "1.2.3.4::1", "::1.2.3", "::01.2.3.4", "1:2:3:4:5:6:7::8", "::1.2.3.4:1"})
        CHECK(!parseIpv6(bad).has_value());
}
