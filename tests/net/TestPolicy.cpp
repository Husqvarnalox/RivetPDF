// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "net/Error.hpp"
#include "net/Policy.hpp"

#include <cstdio>

using namespace rivet;
using namespace rivet::net;

namespace {

Url url(const char* text) {
    auto u = parseUrl(text);
    if (!u) std::fprintf(stderr, "    bad test URL: %s\n", text);
    CHECK(u.has_value());
    return *u;
}

NetworkPolicy httpsOnly() { return NetworkPolicy{}; }

} // namespace

RIVET_TEST(netPolicyDefaults) {
    const NetworkPolicy p;
    CHECK(!p.allowHttp);
    CHECK(!p.allowPrivateNetworks);
    CHECK(!p.useSystemProxy);
    CHECK(p.allowedHosts.empty());
    CHECK_EQ(p.maxRedirects, 3);
    CHECK_EQ(p.maxResponseBytes, static_cast<std::size_t>(4 * 1024 * 1024));
    CHECK_EQ(p.connectTimeout.count(), 10'000);
    CHECK_EQ(p.totalTimeout.count(), 30'000);
    CHECK_EQ(p.userAgent, std::string("Rivet"));
}

RIVET_TEST(netPolicyAllowListWildcard) {
    CHECK(hostMatchesPattern("example.com", "example.com"));
    CHECK(hostMatchesPattern("Example.COM", "example.com"));
    CHECK(!hostMatchesPattern("example.com", "www.example.com"));
    CHECK(hostMatchesPattern("*.example.com", "a.example.com"));
    CHECK(hostMatchesPattern("*.example.com", "a.b.example.com"));
    CHECK(!hostMatchesPattern("*.example.com", "example.com"));
    CHECK(!hostMatchesPattern("*.example.com", "example.com.evil.com"));
    CHECK(!hostMatchesPattern("*.example.com", "evilexample.com"));
    CHECK(!hostMatchesPattern("*.example.com", "evil-example.com"));
    CHECK(!hostMatchesPattern("*.example.com", ".example.com"));
    CHECK(!hostMatchesPattern("*.example.com", "xexample.com"));
    CHECK(!hostMatchesPattern("*.", "a."));
    CHECK(!hostMatchesPattern("*", "example.com")); // bare star is not a wildcard
    CHECK(!hostMatchesPattern("", ""));

    NetworkPolicy p;
    p.allowedHosts = {"tsa.example.org", "*.ocsp.example.com"};
    CHECK(hostAllowed(p, "tsa.example.org"));
    CHECK(hostAllowed(p, "x.ocsp.example.com"));
    CHECK(!hostAllowed(p, "ocsp.example.com"));
    CHECK(!hostAllowed(p, "other.example.org"));
    CHECK(checkUrl(url("https://tsa.example.org/"), p).has_value());
    auto denied = checkUrl(url("https://evil.example/"), p);
    CHECK(!denied.has_value());
    CHECK(failureOf(denied.error()) == NetFailure::PolicyViolation);
}

RIVET_TEST(netPolicySchemeAndPrivateNames) {
    auto p = httpsOnly();
    CHECK(checkUrl(url("https://example.com/"), p).has_value());
    auto http = checkUrl(url("http://example.com/"), p);
    CHECK(!http.has_value());
    CHECK(failureOf(http.error()) == NetFailure::PolicyViolation);
    p.allowHttp = true;
    CHECK(checkUrl(url("http://example.com/"), p).has_value());

    for (const char* bad : {"https://localhost/", "https://LOCALHOST/", "https://foo.localhost/", "https://printer.local/",
                            "https://db.internal/", "https://localhost./"}) {
        auto r = checkUrl(url(bad), p);
        if (r) std::fprintf(stderr, "    expected private-name refusal: %s\n", bad);
        CHECK(!r.has_value());
    }
    // Look-alikes that are ordinary public names.
    for (const char* ok : {"https://notlocalhost.example/", "https://localhost.example/", "https://local.example/",
                           "https://internal.example/"}) {
        CHECK(checkUrl(url(ok), p).has_value());
    }
    p.allowPrivateNetworks = true;
    CHECK(checkUrl(url("https://localhost/"), p).has_value());
    CHECK(checkUrl(url("https://127.0.0.1/"), p).has_value());
}

RIVET_TEST(netPolicyIpLiterals) {
    const NetworkPolicy p = [] {
        NetworkPolicy x;
        x.allowHttp = true;
        return x;
    }();
    for (const char* bad : {"http://127.0.0.1/", "http://127.1/", "http://[::1]/", "http://0x7f.0.0.1/",
                            "http://2130706433/", "http://0177.0.0.1/", "http://169.254.169.254/latest/meta-data/",
                            "http://10.0.0.5/", "http://192.168.1.1/", "http://172.16.0.1/", "http://100.64.0.1/",
                            "http://0.0.0.0/", "http://[::ffff:7f00:1]/", "http://[fe80::1]/", "http://[fd00::1]/",
                            "http://[64:ff9b::7f00:1]/", "http://[2002:7f00:1::]/", "http://224.0.0.1/",
                            "http://255.255.255.255/", "http://240.0.0.1/"}) {
        auto r = checkUrl(url(bad), p);
        if (r) std::fprintf(stderr, "    expected SSRF refusal: %s\n", bad);
        CHECK(!r.has_value());
        CHECK(failureOf(r.error()) == NetFailure::ForbiddenAddress);
    }
    CHECK(checkUrl(url("http://8.8.8.8/"), p).has_value());
    CHECK(checkUrl(url("http://[2001:4860:4860::8888]/"), p).has_value());
}

RIVET_TEST(netPolicyHeaders) {
    CHECK(validateHeaders({{"Accept", "application/ocsp-response"}, {"Content-Type", "application/timestamp-query"}}).has_value());
    for (const char* name : {"Cookie", "cookie", "Authorization", "AUTHORIZATION", "Proxy-Authorization", "Referer", "Host",
                             "User-Agent", "Content-Length", "Transfer-Encoding", "Connection", "Expect", "Upgrade"})
        CHECK(!validateHeaders({{name, "x"}}).has_value());
    CHECK(!validateHeaders({{"X-A", "v\r\nX-B: w"}}).has_value());
    CHECK(!validateHeaders({{"X-A", "v\nw"}}).has_value());
    CHECK(!validateHeaders({{"X A", "v"}}).has_value());
    CHECK(!validateHeaders({{"X-A\r\nX-B", "v"}}).has_value());
    CHECK(!validateHeaders({{"", "v"}}).has_value());
}

RIVET_TEST(netPolicyEffectiveLimitsOnlyTighten) {
    NetworkPolicy p;
    HttpRequest r;
    auto l = effectiveLimits(r, p);
    CHECK_EQ(l.maxResponseBytes, p.maxResponseBytes);
    CHECK_EQ(l.totalTimeout.count(), 30'000);
    r.maxResponseBytes = 100;
    r.timeouts.total = std::chrono::milliseconds(500);
    r.timeouts.connect = std::chrono::milliseconds(900);
    l = effectiveLimits(r, p);
    CHECK_EQ(l.maxResponseBytes, static_cast<std::size_t>(100));
    CHECK_EQ(l.totalTimeout.count(), 500);
    CHECK_EQ(l.connectTimeout.count(), 500); // never above total
    r.maxResponseBytes = 1u << 30;
    r.timeouts.total = std::chrono::minutes(10);
    l = effectiveLimits(r, p);
    CHECK_EQ(l.maxResponseBytes, p.maxResponseBytes);
    CHECK_EQ(l.totalTimeout.count(), 30'000);
}

RIVET_TEST(netPolicySystemProxyKeepsTheSsrfInvariant) {
    CHECK(!NetworkPolicy{}.allowPrivateProxy);
    NetworkPolicy p;
    p.useSystemProxy = true;
    // The proxy resolves names, so without an allow-list every host name is refused...
    auto denied = checkUrl(url("https://tsa.example.org/"), p);
    CHECK(!denied.has_value());
    // ...and the private-name / literal checks still apply on top.
    CHECK(!checkUrl(url("https://127.0.0.1/"), p).has_value());
    CHECK(!checkUrl(url("https://localhost/"), p).has_value());
    // Public IP literals can be classified here, so they stay allowed.
    CHECK(checkUrl(url("https://93.184.216.34/"), p).has_value());
    // With an explicit allow-list, listed names pass; others do not.
    p.allowedHosts = {"tsa.example.org"};
    CHECK(checkUrl(url("https://tsa.example.org/"), p).has_value());
    CHECK(!checkUrl(url("https://other.example.org/"), p).has_value());
    CHECK(!checkUrl(url("https://127.0.0.1/"), p).has_value());
    // allowPrivateProxy never relaxes the destination checks.
    p.allowPrivateProxy = true;
    CHECK(!checkUrl(url("https://127.0.0.1/"), p).has_value());
    // Without a proxy nothing changes: a name needs no allow-list.
    NetworkPolicy plain;
    CHECK(checkUrl(url("https://tsa.example.org/"), plain).has_value());
}
