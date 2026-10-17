// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rivet::net {

// A parsed IPv4 or IPv6 address (network byte order). IPv4 uses bytes[0..3].
struct IpAddress {
    bool isV6 = false;
    std::array<std::uint8_t, 16> bytes{};

    static IpAddress v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d);
    static IpAddress v6(const std::array<std::uint8_t, 16>& raw);

    bool operator==(const IpAddress&) const = default;
};

// Strict dotted-decimal IPv4 ("1.2.3.4": four decimal parts, each 0..255).
std::optional<IpAddress> parseIpv4Strict(std::string_view text);

// inet_aton-style IPv4: 1..4 parts, each decimal, 0x-hex or 0-octal; the last
// part fills the remaining bytes ("127.1", "0x7f.1", "0177.0.0.1",
// "2130706433"). This is what browsers and C libraries accept, so a URL host
// in this form MUST be classified as the address it denotes, never forwarded
// to a resolver as a name.
std::optional<IpAddress> parseIpv4Legacy(std::string_view text);

// IPv6 text form without brackets or zone id ("::1", "::ffff:1.2.3.4").
std::optional<IpAddress> parseIpv6(std::string_view text);

// Canonical text: dotted decimal, or lowercase compressed IPv6.
std::string formatIp(const IpAddress& address);

// SSRF guard: true for every address Rivet must never connect to unless the
// policy explicitly allows private networks: loopback, private, link-local
// (cloud metadata 169.254.0.0/16), CGNAT, unspecified, multicast, broadcast,
// reserved, documentation and benchmark ranges, IPv6 ULA / link-local /
// site-local / multicast, and the IPv4-in-IPv6 forms (::ffff:a.b.c.d,
// 64:ff9b::/96, 2002::/16 6to4, IPv4-compatible ::a.b.c.d, Teredo) whose
// embedded IPv4 address is classified.
bool isForbiddenAddress(const IpAddress& address);

} // namespace rivet::net
