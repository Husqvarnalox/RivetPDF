// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "net/Ip.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rivet::net {

inline constexpr std::size_t kMaxUrlLength = 2048;

// A validated absolute http(s) URL in canonical form.
struct Url {
    std::string scheme;           // "http" | "https"
    std::string host;             // lowercase; dotted IPv4 / compressed IPv6 (no brackets) for literals
    std::uint16_t port = 0;       // effective port (80/443 when not written)
    std::string target = "/";     // path + optional "?query", never a fragment
    std::optional<IpAddress> ip;  // set when the host is an IP literal (any accepted notation)

    bool isHttps() const { return scheme == "https"; }
    // "scheme://host[:port]" (default port omitted, IPv6 bracketed).
    std::string origin() const;
    std::string toString() const;

    bool operator==(const Url&) const = default;
};

// Strict URL parser. Accepts only http/https; rejects userinfo, backslashes,
// whitespace/control/non-ASCII bytes, percent-escapes or unusual characters in
// the host, empty/invalid ports, IDN hosts that are not already punycode and
// URLs over kMaxUrlLength. Fragments are stripped, the host is lowercased, one
// trailing dot is removed, and numeric host forms (127.1, 0x7f.0.0.1,
// 2130706433, [::1]) are canonicalised into Url::ip so policy classifies the
// real address instead of handing an ambiguous name to a resolver.
core::Result<Url> parseUrl(std::string_view text);

// RFC 3986 reference resolution against `base` (for redirects). The result is
// re-validated with the same rules as parseUrl.
core::Result<Url> resolveReference(const Url& base, std::string_view reference);

} // namespace rivet::net
