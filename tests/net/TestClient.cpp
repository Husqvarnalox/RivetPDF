// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "net/Client.hpp"
#include "net/MockTransport.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace rivet;
using namespace rivet::net;

namespace {

struct Fixture {
    std::shared_ptr<MockTransport> mock = std::make_shared<MockTransport>();
    NetworkPolicy policy;
    std::shared_ptr<OnlineValidationGate> gate =
        std::make_shared<OnlineValidationGate>(OnlineValidationMode::On);

    NetworkClient client() const { return NetworkClient(policy, mock, gate); }
};

HttpRequest get(const char* text) {
    HttpRequest r;
    auto u = parseUrl(text);
    if (!u) std::fprintf(stderr, "    bad test URL: %s\n", text);
    CHECK(u.has_value());
    r.url = *u;
    return r;
}

MockReply ok(std::string body = "hello") {
    MockReply r;
    r.body = std::move(body);
    return r;
}

void expectFailure(const core::Result<HttpResponse>& r, NetFailure expected) {
    if (r) {
        std::fprintf(stderr, "    expected failure %s but got status %d\n", std::string(toString(expected)).c_str(), r->status);
    } else if (failureOf(r.error()) != expected) {
        std::fprintf(stderr, "    expected %s, got %s\n", std::string(toString(expected)).c_str(), describe(r.error()).c_str());
    }
    CHECK(!r.has_value());
    CHECK(failureOf(r.error()) == expected);
}

} // namespace

RIVET_TEST(netClientBasicGet) {
    Fixture f;
    f.mock->script("https://example.com/x", ok("payload"));
    auto r = f.client().fetch(get("https://example.com/x"));
    CHECK(r.has_value());
    CHECK_EQ(r->status, 200);
    CHECK_EQ(r->body, std::string("payload"));
    CHECK_EQ(r->finalUrl.toString(), std::string("https://example.com/x"));
    CHECK_EQ(f.mock->callCount(), static_cast<std::size_t>(1));
}

RIVET_TEST(netClientRejectsBadRequests) {
    Fixture f;
    f.mock->script("https://example.com/x", ok());
    auto withBody = get("https://example.com/x");
    withBody.body = "x";
    expectFailure(f.client().fetch(withBody), NetFailure::InvalidRequest);
    auto cookie = get("https://example.com/x");
    cookie.headers.emplace_back("Cookie", "a=b");
    expectFailure(f.client().fetch(cookie), NetFailure::InvalidRequest);
    auto inject = get("https://example.com/x");
    inject.headers.emplace_back("X-A", "v\r\nX-B: 1");
    expectFailure(f.client().fetch(inject), NetFailure::InvalidRequest);
    CHECK_EQ(f.mock->callCount(), static_cast<std::size_t>(0));
}

RIVET_TEST(netClientRefusesPolicyViolationsBeforeTransport) {
    Fixture f;
    f.mock->script("http://example.com/", ok());
    expectFailure(f.client().fetch(get("http://example.com/")), NetFailure::PolicyViolation); // http not allowed
    expectFailure(f.client().fetch(get("https://localhost/")), NetFailure::PolicyViolation);
    expectFailure(f.client().fetch(get("https://127.0.0.1/")), NetFailure::ForbiddenAddress);
    expectFailure(f.client().fetch(get("https://[::1]/")), NetFailure::ForbiddenAddress);
    expectFailure(f.client().fetch(get("http://2130706433/")), NetFailure::PolicyViolation); // scheme first
    f.policy.allowHttp = true;
    expectFailure(f.client().fetch(get("http://2130706433/")), NetFailure::ForbiddenAddress);
    expectFailure(f.client().fetch(get("http://169.254.169.254/latest/meta-data")), NetFailure::ForbiddenAddress);
    CHECK_EQ(f.mock->callCount(), static_cast<std::size_t>(0));
}

RIVET_TEST(netClientSystemProxyDoesNotBypassDestinationPolicy) {
    Fixture f;
    f.policy.useSystemProxy = true;
    f.mock->script("https://tsa.example.org/", ok());
    f.mock->script("https://127.0.0.1/", ok());
    expectFailure(f.client().fetch(get("https://tsa.example.org/")), NetFailure::PolicyViolation); // no allow-list
    expectFailure(f.client().fetch(get("https://127.0.0.1/")), NetFailure::ForbiddenAddress);
    CHECK_EQ(f.mock->callCount(), static_cast<std::size_t>(0));
    f.policy.allowedHosts = {"tsa.example.org"};
    CHECK(f.client().fetch(get("https://tsa.example.org/")).has_value());
    expectFailure(f.client().fetch(get("https://evil.example.net/")), NetFailure::PolicyViolation);
    CHECK_EQ(f.mock->callCount(), static_cast<std::size_t>(1));
}

RIVET_TEST(netClientConnectTimeAddressGuard) {
    // A public name that "resolves" to a private address: DNS rebinding.
    Fixture f;
    MockReply rebinding = ok();
    rebinding.connectAddress = IpAddress::v4(169, 254, 169, 254);
    f.mock->script("https://rebind.example/", rebinding);
    expectFailure(f.client().fetch(get("https://rebind.example/")), NetFailure::ForbiddenAddress);
    MockReply mapped = ok();
    mapped.connectAddress = parseIpv6("::ffff:10.0.0.1");
    f.mock->script("https://mapped.example/", mapped);
    expectFailure(f.client().fetch(get("https://mapped.example/")), NetFailure::ForbiddenAddress);
    // With private networks explicitly allowed the same connection proceeds.
    f.policy.allowPrivateNetworks = true;
    CHECK(f.client().fetch(get("https://rebind.example/")).has_value());
}

RIVET_TEST(netClientFollowsRedirectsAndRecordsHops) {
    Fixture f;
    f.mock->scriptRedirect("https://a.example/1", "/2");
    f.mock->scriptRedirect("https://a.example/2", "https://b.example/3", 307);
    f.mock->script("https://b.example/3", ok("done"));
    auto r = f.client().fetch(get("https://a.example/1"));
    CHECK(r.has_value());
    CHECK_EQ(r->body, std::string("done"));
    CHECK_EQ(r->finalUrl.toString(), std::string("https://b.example/3"));
    CHECK_EQ(f.mock->callCount(), static_cast<std::size_t>(3));
    const auto reqs = f.mock->requests();
    CHECK(reqs[0].maxRedirects.value_or(-1) == 0); // transport never follows
}

RIVET_TEST(netClientRedirectLimit) {
    Fixture f;
    f.policy.maxRedirects = 3;
    f.mock->scriptRedirect("https://loop.example/a", "/b");
    f.mock->scriptRedirect("https://loop.example/b", "/a");
    expectFailure(f.client().fetch(get("https://loop.example/a")), NetFailure::RedirectLimit);
    CHECK_EQ(f.mock->callCount(), static_cast<std::size_t>(4)); // initial + 3 hops, then refused

    // Exactly maxRedirects hops is fine.
    Fixture g;
    g.mock->scriptRedirect("https://c.example/1", "/2");
    g.mock->scriptRedirect("https://c.example/2", "/3");
    g.mock->scriptRedirect("https://c.example/3", "/4");
    g.mock->script("https://c.example/4", ok());
    CHECK(g.client().fetch(get("https://c.example/1")).has_value());

    // A request can lower, never raise, the limit.
    auto strict = get("https://c.example/1");
    strict.maxRedirects = 0;
    expectFailure(g.client().fetch(strict), NetFailure::RedirectLimit);
    g.policy.maxRedirects = 1;
    auto greedy = get("https://c.example/1");
    greedy.maxRedirects = 10;
    expectFailure(g.client().fetch(greedy), NetFailure::RedirectLimit);
}

RIVET_TEST(netClientRevalidatesEveryRedirectHop) {
    Fixture f;
    f.policy.allowHttp = true;
    // Redirect to private IP literals in every notation, to localhost, and to metadata.
    for (const char* target : {"https://127.0.0.1/admin", "https://169.254.169.254/latest/meta-data/", "https://[::1]/",
                               "https://2130706433/", "https://10.0.0.1/", "https://localhost/", "//192.168.0.1/x",
                               "https://0x7f.1/"}) {
        f.mock->scriptRedirect("https://public.example/r", target);
        auto r = f.client().fetch(get("https://public.example/r"));
        if (r) std::fprintf(stderr, "    redirect to %s was followed\n", target);
        CHECK(!r.has_value());
        const auto kind = failureOf(r.error());
        CHECK(kind == NetFailure::ForbiddenAddress || kind == NetFailure::PolicyViolation);
    }
    // The private target was never contacted.
    for (const auto& req : f.mock->requests()) CHECK(req.url.host == "public.example");

    // Redirect to a public host whose connection lands on a private address.
    MockReply rebinding = ok();
    rebinding.connectAddress = IpAddress::v4(10, 1, 1, 1);
    f.mock->script("https://rebind.example/", rebinding);
    f.mock->scriptRedirect("https://public.example/r", "https://rebind.example/");
    expectFailure(f.client().fetch(get("https://public.example/r")), NetFailure::ForbiddenAddress);
}

RIVET_TEST(netClientRefusesHttpsToHttpDowngrade) {
    Fixture f;
    f.policy.allowHttp = true; // http itself is permitted, the downgrade still is not
    f.mock->scriptRedirect("https://secure.example/", "http://secure.example/plain");
    f.mock->script("http://secure.example/plain", ok());
    expectFailure(f.client().fetch(get("https://secure.example/")), NetFailure::RedirectDowngrade);
    CHECK_EQ(f.mock->callCount(), static_cast<std::size_t>(1));
    // http -> https upgrade is fine, and http -> http is permitted by this policy.
    f.mock->scriptRedirect("http://up.example/", "https://up.example/s");
    f.mock->script("https://up.example/s", ok());
    CHECK(f.client().fetch(get("http://up.example/")).has_value());
}

RIVET_TEST(netClientRedirectToDisallowedHostRejected) {
    Fixture f;
    f.policy.allowedHosts = {"good.example", "*.cdn.example"};
    f.mock->scriptRedirect("https://good.example/", "https://evil.example/");
    expectFailure(f.client().fetch(get("https://good.example/")), NetFailure::PolicyViolation);
    f.mock->scriptRedirect("https://good.example/", "https://cdn.example/"); // apex does not match *.cdn.example
    expectFailure(f.client().fetch(get("https://good.example/")), NetFailure::PolicyViolation);
    f.mock->scriptRedirect("https://good.example/", "https://cdn.example.evil.example/");
    expectFailure(f.client().fetch(get("https://good.example/")), NetFailure::PolicyViolation);
    f.mock->scriptRedirect("https://good.example/", "https://a.cdn.example/ok");
    f.mock->script("https://a.cdn.example/ok", ok());
    CHECK(f.client().fetch(get("https://good.example/")).has_value());
}

RIVET_TEST(netClientBadRedirectTargets) {
    Fixture f;
    for (const char* target : {"file:///etc/passwd", "ftp://example.com/", "javascript:alert(1)", "https://u:p@example.com/",
                               "https://example.com/a b", "https://[::1"}) {
        f.mock->scriptRedirect("https://r.example/", target);
        auto r = f.client().fetch(get("https://r.example/"));
        if (r) std::fprintf(stderr, "    followed bad redirect %s\n", target);
        CHECK(!r.has_value());
    }
    // A 3xx without Location is returned as an ordinary response.
    MockReply plain;
    plain.status = 302;
    f.mock->script("https://nolocation.example/", plain);
    auto r = f.client().fetch(get("https://nolocation.example/"));
    CHECK(r.has_value());
    CHECK_EQ(r->status, 302);
    // 304 is not a redirect even with a Location header.
    MockReply notModified;
    notModified.status = 304;
    notModified.headers.emplace_back("Location", "https://elsewhere.example/");
    f.mock->script("https://nm.example/", notModified);
    CHECK_EQ(f.client().fetch(get("https://nm.example/"))->status, 304);
}

RIVET_TEST(netClientRedirectMethodHandling) {
    Fixture f;
    f.mock->scriptRedirect("https://p.example/post", "/get", 303);
    f.mock->script("https://p.example/get", ok());
    auto req = get("https://p.example/post");
    req.method = HttpMethod::Post;
    req.body = "secret-body";
    CHECK(f.client().fetch(req).has_value());
    auto reqs = f.mock->requests();
    CHECK(reqs[0].method == HttpMethod::Post);
    CHECK(reqs[1].method == HttpMethod::Get);
    CHECK(reqs[1].body.empty());

    Fixture g;
    g.mock->scriptRedirect("https://p.example/post", "/again", 307);
    g.mock->script("https://p.example/again", ok());
    CHECK(g.client().fetch(req).has_value());
    reqs = g.mock->requests();
    CHECK(reqs[1].method == HttpMethod::Post);
    CHECK_EQ(reqs[1].body, std::string("secret-body"));
}

RIVET_TEST(netClientOversizeResponseRejected) {
    Fixture f;
    f.policy.maxResponseBytes = 1000;
    f.mock->script("https://big.example/", ok(std::string(1001, 'x')));
    f.mock->script("https://fit.example/", ok(std::string(1000, 'x')));
    expectFailure(f.client().fetch(get("https://big.example/")), NetFailure::ResponseTooLarge);
    CHECK(f.client().fetch(get("https://fit.example/")).has_value());
    // A request may lower the cap but not raise it.
    auto small = get("https://fit.example/");
    small.maxResponseBytes = 10;
    expectFailure(f.client().fetch(small), NetFailure::ResponseTooLarge);
    auto huge = get("https://big.example/");
    huge.maxResponseBytes = std::size_t{1} << 30;
    expectFailure(f.client().fetch(huge), NetFailure::ResponseTooLarge);

    // A transport that ignores the cap is still caught by the client.
    struct Sloppy final : INetworkTransport {
        core::Result<HttpResponse> perform(const HttpRequest& r, const NetworkPolicy&, CancelToken) override {
            HttpResponse resp;
            resp.status = 200;
            resp.body = std::string(5000, 'z');
            resp.finalUrl = r.url;
            return resp;
        }
    };
    NetworkClient sloppy(f.policy, std::make_shared<Sloppy>(), f.gate);
    expectFailure(sloppy.fetch(get("https://any.example/")), NetFailure::ResponseTooLarge);
}

RIVET_TEST(netClientTimeouts) {
    Fixture f;
    f.policy.totalTimeout = std::chrono::milliseconds(150);
    MockReply slow = ok();
    slow.latency = std::chrono::seconds(5);
    f.mock->script("https://slow.example/", slow);
    expectFailure(f.client().fetch(get("https://slow.example/")), NetFailure::Timeout);

    MockReply hang = ok();
    hang.hang = true;
    f.mock->script("https://hang.example/", hang);
    const auto start = std::chrono::steady_clock::now();
    expectFailure(f.client().fetch(get("https://hang.example/")), NetFailure::Timeout);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));

    // The total budget spans the whole redirect chain.
    Fixture g;
    g.policy.totalTimeout = std::chrono::milliseconds(120);
    MockReply step = ok();
    step.status = 302;
    step.headers.emplace_back("Location", "/b");
    step.latency = std::chrono::milliseconds(80);
    g.mock->script("https://chain.example/a", step);
    MockReply second = ok();
    second.latency = std::chrono::milliseconds(80);
    g.mock->script("https://chain.example/b", second);
    expectFailure(g.client().fetch(get("https://chain.example/a")), NetFailure::Timeout);

    // The hop receives the remaining budget, never more than the policy.
    Fixture h;
    h.mock->script("https://t.example/", ok());
    auto timed = get("https://t.example/");
    timed.timeouts.total = std::chrono::milliseconds(5000);
    CHECK(h.client().fetch(timed).has_value());
    const auto hop = h.mock->requests().front();
    CHECK(hop.timeouts.total.count() > 0 && hop.timeouts.total.count() <= 5000);
    CHECK(hop.timeouts.connect.count() > 0 && hop.timeouts.connect <= hop.timeouts.total);
}

RIVET_TEST(netClientCancellation) {
    Fixture f;
    MockReply hang = ok();
    hang.hang = true;
    f.mock->script("https://hang.example/", hang);
    CancelToken cancel;
    std::thread canceller([cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        cancel.cancel();
    });
    const auto start = std::chrono::steady_clock::now();
    auto r = f.client().fetch(get("https://hang.example/"), cancel);
    canceller.join();
    expectFailure(r, NetFailure::Cancelled);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));

    // Already-cancelled: transport is never called.
    Fixture g;
    g.mock->script("https://example.com/", ok());
    CancelToken pre;
    pre.cancel();
    expectFailure(g.client().fetch(get("https://example.com/"), pre), NetFailure::Cancelled);
    CHECK_EQ(g.mock->callCount(), static_cast<std::size_t>(0));

    // Cancelled while the transport "completes": result is discarded.
    struct CancelsWhileWorking final : INetworkTransport {
        core::Result<HttpResponse> perform(const HttpRequest& r, const NetworkPolicy&, CancelToken c) override {
            c.cancel();
            HttpResponse resp;
            resp.status = 200;
            resp.finalUrl = r.url;
            return resp;
        }
    };
    NetworkClient late(g.policy, std::make_shared<CancelsWhileWorking>(), g.gate);
    CancelToken shared;
    expectFailure(late.fetch(get("https://example.com/"), shared), NetFailure::Cancelled);
}

RIVET_TEST(netClientNoSensitiveHeadersSent) {
    Fixture f;
    f.mock->script("https://example.com/", ok());
    auto req = get("https://example.com/");
    req.headers.emplace_back("Accept", "application/octet-stream");
    CHECK(f.client().fetch(req).has_value());
    for (const auto& [name, value] : f.mock->requests().front().headers) {
        (void)value;
        CHECK(name != "Cookie" && name != "Authorization" && name != "Referer");
    }
}

RIVET_TEST(netDefaultTransportUnavailableWhenBackendOff) {
    if (networkBackendAvailable()) return; // exercised by the libcurl tests instead
    NetworkClient client(NetworkPolicy{}, nullptr, std::make_shared<OnlineValidationGate>(OnlineValidationMode::On));
    expectFailure(client.fetch(get("https://example.com/")), NetFailure::BackendUnavailable);
}
