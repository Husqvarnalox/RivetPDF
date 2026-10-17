// SPDX-License-Identifier: MPL-2.0
#include "net/Ip.hpp"

#include <cstddef>
#include <cstdio>
#include <vector>

namespace rivet::net {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<std::string_view> split(std::string_view text, char sep) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = text.find(sep, start);
        if (pos == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

// Parses one inet_aton component. `allowRadix` enables 0x / leading-0 forms.
std::optional<std::uint64_t> parseComponent(std::string_view part, bool allowRadix) {
    if (part.empty()) return std::nullopt;
    unsigned base = 10;
    if (allowRadix && part.size() >= 2 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X')) {
        base = 16;
        part.remove_prefix(2);
        if (part.empty()) return std::nullopt;
    } else if (allowRadix && part.size() >= 2 && part[0] == '0') {
        base = 8;
        part.remove_prefix(1);
    }
    std::uint64_t value = 0;
    for (char c : part) {
        const int digit = hexValue(c);
        if (digit < 0 || static_cast<unsigned>(digit) >= base) return std::nullopt;
        value = value * base + static_cast<unsigned>(digit);
        if (value > 0xFFFFFFFFull) return std::nullopt;
    }
    return value;
}

bool forbiddenV4(const IpAddress& a) {
    const auto b0 = a.bytes[0];
    const auto b1 = a.bytes[1];
    const auto b2 = a.bytes[2];
    if (b0 == 0) return true;                       // 0.0.0.0/8 "this network"
    if (b0 == 10) return true;                      // 10/8
    if (b0 == 100 && (b1 & 0xC0) == 64) return true; // 100.64/10 CGNAT
    if (b0 == 127) return true;                     // loopback
    if (b0 == 169 && b1 == 254) return true;        // link-local, cloud metadata
    if (b0 == 172 && (b1 & 0xF0) == 16) return true; // 172.16/12
    if (b0 == 192 && b1 == 0 && b2 == 0) return true; // 192.0.0.0/24 IETF protocol
    if (b0 == 192 && b1 == 0 && b2 == 2) return true; // TEST-NET-1
    if (b0 == 192 && b1 == 88 && b2 == 99) return true; // 6to4 relay anycast (reserved)
    if (b0 == 192 && b1 == 168) return true;        // 192.168/16
    if (b0 == 198 && (b1 & 0xFE) == 18) return true; // 198.18/15 benchmarking
    if (b0 == 198 && b1 == 51 && b2 == 100) return true; // TEST-NET-2
    if (b0 == 203 && b1 == 0 && b2 == 113) return true;  // TEST-NET-3
    if (b0 >= 224) return true;                     // multicast, 240/4 reserved, broadcast
    return false;
}

IpAddress embeddedV4(const IpAddress& a, std::size_t offset) {
    return IpAddress::v4(a.bytes[offset], a.bytes[offset + 1], a.bytes[offset + 2], a.bytes[offset + 3]);
}

bool forbiddenV6(const IpAddress& a) {
    const auto& b = a.bytes;
    auto zeros = [&](std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; ++i)
            if (b[i] != 0) return false;
        return true;
    };
    if (zeros(0, 15) && (b[15] == 0 || b[15] == 1)) return true; // :: and ::1
    if (zeros(0, 10) && b[10] == 0xFF && b[11] == 0xFF) return forbiddenV4(embeddedV4(a, 12)); // ::ffff:a.b.c.d
    if (zeros(0, 12)) return forbiddenV4(embeddedV4(a, 12));    // IPv4-compatible (deprecated)
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xFF && b[3] == 0x9B) {
        if (zeros(4, 12)) return forbiddenV4(embeddedV4(a, 12)); // 64:ff9b::/96 NAT64
        if (b[4] == 0x00 && b[5] == 0x01) return true;           // 64:ff9b:1::/48 local-use NAT64
    }
    if (b[0] == 0x01 && zeros(1, 8)) return true; // 100::/64 discard (0100::)
    if (b[0] == 0x20 && b[1] == 0x02) return forbiddenV4(embeddedV4(a, 2)); // 2002::/16 6to4
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00) return true; // Teredo 2001::/32
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0D && b[3] == 0xB8) return true; // documentation
    if ((b[0] & 0xFE) == 0xFC) return true;                    // fc00::/7 ULA
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return true;    // fe80::/10 link-local
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0xC0) return true;    // fec0::/10 site-local
    if (b[0] == 0xFF) return true;                             // multicast
    return false;
}

} // namespace

IpAddress IpAddress::v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    IpAddress r;
    r.bytes[0] = a;
    r.bytes[1] = b;
    r.bytes[2] = c;
    r.bytes[3] = d;
    return r;
}

IpAddress IpAddress::v6(const std::array<std::uint8_t, 16>& raw) {
    IpAddress r;
    r.isV6 = true;
    r.bytes = raw;
    return r;
}

std::optional<IpAddress> parseIpv4Strict(std::string_view text) {
    const auto parts = split(text, '.');
    if (parts.size() != 4) return std::nullopt;
    std::array<std::uint8_t, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& p = parts[i];
        if (p.empty() || p.size() > 3) return std::nullopt;
        if (p.size() > 1 && p[0] == '0') return std::nullopt; // no leading zeros (octal ambiguity)
        unsigned value = 0;
        for (char c : p) {
            if (!isDigit(c)) return std::nullopt;
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        if (value > 255) return std::nullopt;
        out[i] = static_cast<std::uint8_t>(value);
    }
    return IpAddress::v4(out[0], out[1], out[2], out[3]);
}

std::optional<IpAddress> parseIpv4Legacy(std::string_view text) {
    const auto parts = split(text, '.');
    if (parts.empty() || parts.size() > 4) return std::nullopt;
    std::vector<std::uint64_t> values;
    for (const auto& p : parts) {
        const auto v = parseComponent(p, true);
        if (!v) return std::nullopt;
        values.push_back(*v);
    }
    const std::size_t n = values.size();
    for (std::size_t i = 0; i + 1 < n; ++i)
        if (values[i] > 255) return std::nullopt;
    const std::uint64_t lastLimit = 1ull << (8 * (5 - n)); // n=1 -> 2^32, n=4 -> 2^8
    if (values[n - 1] >= lastLimit) return std::nullopt;
    std::uint32_t addr = static_cast<std::uint32_t>(values[n - 1]);
    for (std::size_t i = 0; i + 1 < n; ++i) addr |= static_cast<std::uint32_t>(values[i]) << (8 * (3 - i));
    return IpAddress::v4(static_cast<std::uint8_t>(addr >> 24), static_cast<std::uint8_t>(addr >> 16),
                         static_cast<std::uint8_t>(addr >> 8), static_cast<std::uint8_t>(addr));
}

std::optional<IpAddress> parseIpv6(std::string_view text) {
    if (text.empty() || text.size() > 45) return std::nullopt;
    for (char c : text)
        if (!(hexValue(c) >= 0 || c == ':' || c == '.')) return std::nullopt;

    std::size_t dbl = text.find("::");
    if (dbl != std::string_view::npos && text.find("::", dbl + 1) != std::string_view::npos) return std::nullopt;

    auto parseGroups = [](std::string_view part, bool allowV4Tail, std::vector<std::uint16_t>& out) -> bool {
        if (part.empty()) return true;
        const auto groups = split(part, ':');
        for (std::size_t i = 0; i < groups.size(); ++i) {
            const auto g = groups[i];
            if (g.empty()) return false;
            if (g.find('.') != std::string_view::npos) {
                if (!allowV4Tail || i + 1 != groups.size()) return false;
                const auto v4 = parseIpv4Strict(g);
                if (!v4) return false;
                out.push_back(static_cast<std::uint16_t>((v4->bytes[0] << 8) | v4->bytes[1]));
                out.push_back(static_cast<std::uint16_t>((v4->bytes[2] << 8) | v4->bytes[3]));
                continue;
            }
            if (g.size() > 4) return false;
            unsigned v = 0;
            for (char c : g) v = v * 16 + static_cast<unsigned>(hexValue(c));
            out.push_back(static_cast<std::uint16_t>(v));
        }
        return true;
    };

    std::vector<std::uint16_t> head, tail;
    if (dbl == std::string_view::npos) {
        if (!parseGroups(text, true, head) || head.size() != 8) return std::nullopt;
    } else {
        const auto left = text.substr(0, dbl);
        const auto right = text.substr(dbl + 2);
        if (!parseGroups(left, false, head)) return std::nullopt;
        if (!parseGroups(right, true, tail)) return std::nullopt;
        if (head.size() + tail.size() > 7) return std::nullopt; // "::" must stand for >=1 group
        head.resize(8 - tail.size(), 0);
        head.insert(head.end(), tail.begin(), tail.end());
    }
    std::array<std::uint8_t, 16> raw{};
    for (std::size_t i = 0; i < 8; ++i) {
        raw[2 * i] = static_cast<std::uint8_t>(head[i] >> 8);
        raw[2 * i + 1] = static_cast<std::uint8_t>(head[i] & 0xFF);
    }
    return IpAddress::v6(raw);
}

std::string formatIp(const IpAddress& a) {
    char buf[64];
    if (!a.isV6) {
        std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", a.bytes[0], a.bytes[1], a.bytes[2], a.bytes[3]);
        return buf;
    }
    std::array<unsigned, 8> g{};
    for (std::size_t i = 0; i < 8; ++i) g[i] = (static_cast<unsigned>(a.bytes[2 * i]) << 8) | a.bytes[2 * i + 1];
    // Longest run of zero groups (length >= 2) is compressed.
    std::size_t bestStart = 0, bestLen = 0;
    for (std::size_t i = 0; i < 8;) {
        if (g[i] != 0) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < 8 && g[j] == 0) ++j;
        if (j - i > bestLen) {
            bestStart = i;
            bestLen = j - i;
        }
        i = j;
    }
    if (bestLen < 2) bestLen = 0;
    std::string out;
    for (std::size_t i = 0; i < 8; ++i) {
        if (bestLen != 0 && i == bestStart) {
            out += "::";
            i += bestLen - 1;
            continue;
        }
        if (!out.empty() && out.back() != ':') out += ':';
        std::snprintf(buf, sizeof buf, "%x", g[i]);
        out += buf;
    }
    return out;
}

bool isForbiddenAddress(const IpAddress& address) { return address.isV6 ? forbiddenV6(address) : forbiddenV4(address); }

} // namespace rivet::net
