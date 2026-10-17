// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "net/Http.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::net {

// What the network layer is allowed to do. Defaults are the safe values; there
// is deliberately no option to disable certificate verification, send cookies,
// forward credentials or add a Referer.
struct NetworkPolicy {
    bool allowHttp = false;             // false: https only
    bool allowPrivateNetworks = false;  // false: SSRF guard on (loopback, private, link-local, ...)
    bool useSystemProxy = false;        // false: libcurl proxy environment variables are ignored
    std::vector<std::string> allowedHosts; // empty: any public host; entries "host" or "*.suffix"
    int maxRedirects = 3;
    std::size_t maxResponseBytes = 4u * 1024u * 1024u;
    std::chrono::milliseconds connectTimeout{10'000};
    std::chrono::milliseconds totalTimeout{30'000};
    std::string userAgent = "Rivet";    // fixed: no version / OS detail
};

// Exact (case-insensitive) or "*.suffix" match. "*.example.com" matches
// "a.example.com" and "a.b.example.com" but NOT "example.com",
// "evilexample.com" or "example.com.evil.com".
bool hostMatchesPattern(std::string_view pattern, std::string_view host);
bool hostAllowed(const NetworkPolicy& policy, std::string_view host);

// "localhost", "*.localhost", "*.local", "*.internal".
bool isPrivateHostName(std::string_view host);

// Individual checks (all return a PolicyViolation / ForbiddenAddress error).
core::Status checkScheme(const Url& url, const NetworkPolicy& policy);
core::Status checkAllowList(const Url& url, const NetworkPolicy& policy);
// Private host names and IP-literal hosts (the part a transport cannot see
// before resolving; hostnames are checked again at connect time).
core::Status checkHostAddress(const Url& url, const NetworkPolicy& policy);
// All of the above, in order.
core::Status checkUrl(const Url& url, const NetworkPolicy& policy);

// Request limits after clamping to the policy.
struct EffectiveLimits {
    std::size_t maxResponseBytes;
    std::chrono::milliseconds connectTimeout;
    std::chrono::milliseconds totalTimeout;
};
EffectiveLimits effectiveLimits(const HttpRequest& request, const NetworkPolicy& policy);

// Headers the caller may not set (the layer owns them or they would leak
// credentials): Host, User-Agent, Cookie, Authorization, Proxy-Authorization,
// Referer, Content-Length, Transfer-Encoding, Connection, Upgrade, Expect.
core::Status validateHeaders(const HttpHeaders& headers);

} // namespace rivet::net
