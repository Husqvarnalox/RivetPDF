// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "net/Url.hpp"

#include <cstdio>
#include <string>

using namespace rivet;
using namespace rivet::net;

namespace {

struct Accept {
    const char* input;
    const char* canonical; // Url::toString()
};

} // namespace

RIVET_TEST(netUrlAccepts) {
    const Accept table[] = {
        {"https://example.com", "https://example.com/"},
        {"HTTPS://EXAMPLE.Com/Path?Q=1", "https://example.com/Path?Q=1"},
        {"http://example.com:8080/a/b", "http://example.com:8080/a/b"},
        {"https://example.com:443/", "https://example.com/"},
        {"https://example.com#frag", "https://example.com/"},
        {"https://example.com/a?b=c#frag", "https://example.com/a?b=c"},
        {"https://example.com?x=1", "https://example.com/?x=1"},
        {"https://example.com./x", "https://example.com/x"},                 // trailing dot stripped
        {"https://xn--bcher-kva.example/", "https://xn--bcher-kva.example/"}, // punycode IDN
        {"https://[2001:4860:4860::8888]/dns", "https://[2001:4860:4860::8888]/dns"},
        {"https://[2001:4860:4860:0:0:0:0:8888]:8443/", "https://[2001:4860:4860::8888]:8443/"},
        {"https://8.8.8.8/", "https://8.8.8.8/"},
        // Numeric forms are canonicalised, not forwarded to DNS.
        {"http://127.1", "http://127.0.0.1/"},
        {"http://[::1]", "http://[::1]/"},
        {"http://0x7f.0.0.1", "http://127.0.0.1/"},
        {"http://2130706433", "http://127.0.0.1/"},
        {"http://0177.0.0.1", "http://127.0.0.1/"},
        {"http://0x7f000001/", "http://127.0.0.1/"},
        {"http://169.254.169.254/latest/meta-data", "http://169.254.169.254/latest/meta-data"},
        {"http://[::ffff:127.0.0.1]/", "http://[::ffff:7f00:1]/"},
    };
    for (const auto& row : table) {
        auto r = parseUrl(row.input);
        if (!r) std::fprintf(stderr, "    expected accept: %s (%s)\n", row.input, describe(r.error()).c_str());
        CHECK(r.has_value());
        const std::string got = r->toString();
        if (got != row.canonical) std::fprintf(stderr, "    %s -> %s (expected %s)\n", row.input, got.c_str(), row.canonical);
        CHECK(got == row.canonical);
    }
}

RIVET_TEST(netUrlNormalisesHostAndLiterals) {
    auto u = parseUrl("https://EXAMPLE.com:8443/x");
    CHECK(u.has_value());
    CHECK_EQ(u->host, std::string("example.com"));
    CHECK_EQ(u->port, 8443);
    CHECK(!u->ip.has_value());
    auto v = parseUrl("http://2130706433/");
    CHECK(v.has_value() && v->ip.has_value());
    CHECK_EQ(v->host, std::string("127.0.0.1"));
    CHECK_EQ(v->port, 80);
}

RIVET_TEST(netUrlRejects) {
    const char* table[] = {
        "",
        "example.com",
        "//example.com/",
        "file:///etc/passwd",
        "ftp://x",
        "ftp://example.com/file",
        "gopher://example.com/",
        "data:text/plain;base64,AAAA",
        "javascript:alert(1)",
        "custom-scheme://example.com/",
        "ws://example.com/",
        "https:example.com",
        "https:/example.com",
        "https://",
        "https:///path",
        "https://user@host/",
        "https://user:pass@host/",
        "https://evil.com\\@good.com",
        "https://good.com\\evil.com/",
        "https://example.com:0/",
        "https://example.com:65536/",
        "https://example.com:99999/",
        "https://example.com:-1/",
        "https://example.com:80a/",
        "https://example.com:/",
        "https://example.com:123456/",
        "https://exa mple.com/",
        "https://example.com/a b",
        " https://example.com/",
        "https://example.com/ ",
        "https://example.com/\r\nHost: evil.com",
        "https://example.com/a\nb",
        "https://example.com/a\tb",
        "https://exam\r\nple.com/",
        "https://b\xC3\xBCr.example/",   // raw IDN (UTF-8)
        "https://example.com/\xC3\xA9",        // raw non-ASCII in path
        "https://exa_mple.com/",
        "https://exa%6Dple.com/",
        "https://-bad.example/",
        "https://bad-.example/",
        "https://a..b.example/",
        "https://example.com../",
        "https://.example.com/",
        "https://[::1/",
        "https://[::1]x/",
        "https://[::1]:/",
        "https://[1:2:3:4:5:6:7:8:9]/",
        "https://[:::1]/",
        "https://[::1%25eth0]/",
        "https://1.2.3.4.5/",
        "https://256.1.1.1/",
        "https://0x/",
        "https://999999999999/",
    };
    for (const char* in : table) {
        const std::string input = in;
        auto r = parseUrl(input);
        if (r) std::fprintf(stderr, "    expected reject: %s -> %s\n", in, r->toString().c_str());
        CHECK(!r.has_value());
    }
}

RIVET_TEST(netUrlRejectsEmbeddedNulAndOverlong) {
    CHECK(!parseUrl(std::string("https://example.com/a\0b", 23)).has_value());
    const std::string longPath(2048, 'a');
    CHECK(!parseUrl("https://example.com/" + longPath).has_value());
    const std::string exactly2048 = "https://example.com/" + std::string(2048 - 20, 'a');
    CHECK_EQ(exactly2048.size(), static_cast<std::size_t>(2048));
    CHECK(parseUrl(exactly2048).has_value());
    CHECK(!parseUrl(exactly2048 + "a").has_value());
    CHECK(!parseUrl("https://" + std::string(64, 'a') + ".example/").has_value()); // label > 63
}

RIVET_TEST(netUrlResolveReference) {
    auto base = parseUrl("https://example.com/a/b/c?x=1");
    CHECK(base.has_value());
    struct Row { const char* ref; const char* expect; };
    const Row table[] = {
        {"d", "https://example.com/a/b/d"},
        {"../d", "https://example.com/a/d"},
        {"../../../../d", "https://example.com/d"},
        {"./d?y=2", "https://example.com/a/b/d?y=2"},
        {"/root", "https://example.com/root"},
        {"?z=3", "https://example.com/a/b/c?z=3"},
        {"//other.example/p", "https://other.example/p"},
        {"https://other.example/p#f", "https://other.example/p"},
        {"http://other.example/", "http://other.example/"},
        {"#only-fragment", "https://example.com/a/b/c?x=1"},
    };
    for (const auto& row : table) {
        auto r = resolveReference(*base, row.ref);
        if (!r) std::fprintf(stderr, "    resolve failed: %s (%s)\n", row.ref, describe(r.error()).c_str());
        CHECK(r.has_value());
        if (r->toString() != row.expect) std::fprintf(stderr, "    %s -> %s (expected %s)\n", row.ref, r->toString().c_str(), row.expect);
        CHECK(r->toString() == row.expect);
    }
    for (const char* bad : {"", "file:///etc/passwd", "javascript:x", "ftp://x/", "a b", "\\\\evil", "//user@evil/", "x\r\ny"}) {
        auto r = resolveReference(*base, bad);
        if (r) std::fprintf(stderr, "    expected reject ref: %s -> %s\n", bad, r->toString().c_str());
        CHECK(!r.has_value());
    }
}
