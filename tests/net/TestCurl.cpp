// SPDX-License-Identifier: MPL-2.0
// Real-transport tests. They never touch the internet: a tiny HTTP server on
// the loopback interface is started inside the test (POSIX only; the tests are
// skipped on Windows and when the libcurl backend is not built).
#include "RivetTest.h"

#include "net/Client.hpp"
#include "net/Transport.hpp"

#if defined(RIVET_NET_CURL) && !defined(_WIN32)

#include "net/curl/CurlTransport.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rivet;
using namespace rivet::net;

namespace {

// Sequential loopback HTTP/1.1 server; one request per connection.
class LoopbackServer {
public:
    using Handler = std::function<std::string(const std::string& rawRequest, const std::atomic<bool>& stopping)>;

    explicit LoopbackServer(Handler handler) : handler_(std::move(handler)) {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(listenFd_ >= 0);
        int yes = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        CHECK(::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        CHECK(::listen(listenFd_, 16) == 0);
        socklen_t len = sizeof addr;
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { loop(); });
    }
    ~LoopbackServer() {
        stopping_ = true;
        if (thread_.joinable()) thread_.join();
        ::close(listenFd_);
    }
    int port() const { return port_; }
    std::string url(const std::string& path, const char* host = "127.0.0.1") const {
        return "http://" + std::string(host) + ":" + std::to_string(port_) + path;
    }
    int connections() const { return connections_.load(); }
    std::vector<std::string> requests() {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    void loop() {
        while (!stopping_) {
            pollfd pfd{listenFd_, POLLIN, 0};
            if (::poll(&pfd, 1, 25) <= 0) continue;
            const int fd = ::accept(listenFd_, nullptr, nullptr);
            if (fd < 0) continue;
            ++connections_;
#ifdef SO_NOSIGPIPE
            int one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            std::string raw;
            char buf[2048];
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (raw.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() < deadline) {
                pollfd rp{fd, POLLIN, 0};
                if (::poll(&rp, 1, 25) <= 0) continue;
                const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
                if (n <= 0) break;
                raw.append(buf, static_cast<std::size_t>(n));
            }
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(raw);
            }
            const std::string response = handler_(raw, stopping_);
            std::size_t sent = 0;
            while (sent < response.size()) {
#ifdef MSG_NOSIGNAL
                const int flags = MSG_NOSIGNAL;
#else
                const int flags = 0;
#endif
                const ssize_t n = ::send(fd, response.data() + sent, response.size() - sent, flags);
                if (n <= 0) break;
                sent += static_cast<std::size_t>(n);
            }
            ::close(fd);
        }
    }

    Handler handler_;
    int listenFd_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> connections_{0};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::string> requests_;
};

std::string httpResponse(int status, const std::string& body, const std::string& extraHeaders = {}) {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
}

std::string pathOf(const std::string& raw) {
    const auto a = raw.find(' ');
    const auto b = raw.find(' ', a + 1);
    return a == std::string::npos || b == std::string::npos ? std::string() : raw.substr(a + 1, b - a - 1);
}

LoopbackServer::Handler standardHandler(const LoopbackServer** self) {
    return [self](const std::string& raw, const std::atomic<bool>& stopping) -> std::string {
        const std::string path = pathOf(raw);
        if (path == "/ok") return httpResponse(200, "hello loopback", "X-Test: yes\r\n");
        if (path == "/big") return httpResponse(200, std::string(200000, 'B'));
        if (path == "/chunky") {
            // No Content-Length: the cap must still stop it.
            std::string out = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
            out += std::string(300000, 'C');
            return out;
        }
        if (path == "/redir") return httpResponse(302, "", "Location: /ok\r\n");
        if (path == "/redir-private") return httpResponse(302, "", "Location: http://10.0.0.1:1/x\r\n");
        if (path == "/loop") return httpResponse(302, "", "Location: /loop\r\n");
        if (path == "/slow") {
            for (int i = 0; i < 100 && !stopping; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return httpResponse(200, "late");
        }
        (void)self;
        return httpResponse(404, "nope");
    };
}

HttpRequest get(const std::string& url) {
    HttpRequest r;
    auto u = parseUrl(url);
    if (!u) std::fprintf(stderr, "    bad URL %s\n", url.c_str());
    CHECK(u.has_value());
    r.url = *u;
    return r;
}

NetworkPolicy loopbackPolicy() {
    NetworkPolicy p;
    p.allowHttp = true;
    p.allowPrivateNetworks = true;
    p.totalTimeout = std::chrono::seconds(10);
    return p;
}

std::shared_ptr<OnlineValidationGate> onGate() {
    return std::make_shared<OnlineValidationGate>(OnlineValidationMode::On);
}

} // namespace

RIVET_TEST(netCurlBackendIsAvailable) {
    CHECK(networkBackendAvailable());
    CHECK(makeDefaultTransport() != nullptr);
}

RIVET_TEST(netCurlHappyPathOnLoopback) {
    LoopbackServer server(standardHandler(nullptr));
    NetworkClient client(loopbackPolicy(), std::make_shared<CurlTransport>(), onGate());
    auto r = client.fetch(get(server.url("/ok")));
    if (!r) std::fprintf(stderr, "    fetch failed: %s\n", describe(r.error()).c_str());
    CHECK(r.has_value());
    CHECK_EQ(r->status, 200);
    CHECK_EQ(r->body, std::string("hello loopback"));
    const auto header = findHeader(r->headers, "x-test");
    CHECK(header.has_value() && *header == "yes");

    // What went over the wire: fixed UA, no cookies / referer / auth.
    const auto reqs = server.requests();
    CHECK_EQ(reqs.size(), static_cast<std::size_t>(1));
    const std::string& wire = reqs[0];
    CHECK(wire.find("User-Agent: Rivet\r\n") != std::string::npos);
    for (const char* forbidden : {"Cookie:", "Referer:", "Authorization:", "Proxy-Authorization:"})
        CHECK(wire.find(forbidden) == std::string::npos);
}

RIVET_TEST(netCurlClientFollowsRedirectItself) {
    LoopbackServer server(standardHandler(nullptr));
    NetworkClient client(loopbackPolicy(), std::make_shared<CurlTransport>(), onGate());
    auto r = client.fetch(get(server.url("/redir")));
    CHECK(r.has_value());
    CHECK_EQ(r->body, std::string("hello loopback"));
    CHECK_EQ(server.connections(), 2);

    // The raw transport is one-hop: it hands back the 302 untouched.
    CurlTransport raw;
    auto hop = raw.perform(get(server.url("/redir")), loopbackPolicy(), CancelToken{});
    CHECK(hop.has_value());
    CHECK_EQ(hop->status, 302);

    auto loop = client.fetch(get(server.url("/loop")));
    CHECK(!loop.has_value());
    CHECK(failureOf(loop.error()) == NetFailure::RedirectLimit);
}

RIVET_TEST(netCurlRedirectToPrivateRefusedByClient) {
    // allowPrivateNetworks=false would refuse the loopback server itself, so
    // allow it for the first hop through an allow-list-free private policy and
    // then verify the redirect target is judged by the *same* policy: with
    // private networks allowed it is attempted (and fails to connect), with
    // them forbidden it is refused before any socket exists.
    LoopbackServer server(standardHandler(nullptr));
    CurlTransport transport;
    auto strict = loopbackPolicy();
    strict.allowPrivateNetworks = false;
    NetworkClient client(strict, std::make_shared<CurlTransport>(), onGate());
    auto r = client.fetch(get(server.url("/redir-private")));
    CHECK(!r.has_value());
    CHECK(failureOf(r.error()) == NetFailure::ForbiddenAddress);
    CHECK_EQ(server.connections(), 0); // not even the first hop was connected
}

RIVET_TEST(netCurlRefusesLoopbackAtConnectTime) {
    LoopbackServer server(standardHandler(nullptr));
    CurlTransport transport;
    auto policy = loopbackPolicy();
    policy.allowPrivateNetworks = false;
    // Called on the transport directly (no client pre-check), so the refusal
    // can only come from the connect-time peer-address check. "localhost" is
    // a name, i.e. what DNS rebinding looks like to the transport.
    for (const char* host : {"127.0.0.1", "localhost"}) {
        auto r = transport.perform(get(server.url("/ok", host)), policy, CancelToken{});
        if (r) std::fprintf(stderr, "    connected to %s despite the guard\n", host);
        CHECK(!r.has_value());
        CHECK(failureOf(r.error()) == NetFailure::ForbiddenAddress);
    }
    CHECK_EQ(server.connections(), 0);
    // Same request is fine once private networks are allowed.
    CHECK(transport.perform(get(server.url("/ok")), loopbackPolicy(), CancelToken{}).has_value());
}

RIVET_TEST(netCurlEnforcesSchemePolicyInTransport) {
    LoopbackServer server(standardHandler(nullptr));
    CurlTransport transport;
    auto policy = loopbackPolicy();
    policy.allowHttp = false;
    auto r = transport.perform(get(server.url("/ok")), policy, CancelToken{});
    CHECK(!r.has_value());
    CHECK(failureOf(r.error()) == NetFailure::PolicyViolation);
    auto listed = loopbackPolicy();
    listed.allowedHosts = {"example.com"};
    auto r2 = transport.perform(get(server.url("/ok")), listed, CancelToken{});
    CHECK(!r2.has_value());
    CHECK(failureOf(r2.error()) == NetFailure::PolicyViolation);
    CHECK_EQ(server.connections(), 0);
}

RIVET_TEST(netCurlResponseSizeCap) {
    LoopbackServer server(standardHandler(nullptr));
    auto policy = loopbackPolicy();
    policy.maxResponseBytes = 100'000;
    NetworkClient client(policy, std::make_shared<CurlTransport>(), onGate());
    auto big = client.fetch(get(server.url("/big")));
    CHECK(!big.has_value());
    CHECK(failureOf(big.error()) == NetFailure::ResponseTooLarge);
    auto chunky = client.fetch(get(server.url("/chunky")));
    CHECK(!chunky.has_value());
    CHECK(failureOf(chunky.error()) == NetFailure::ResponseTooLarge);
    CHECK(client.fetch(get(server.url("/ok"))).has_value());
}

RIVET_TEST(netCurlTotalTimeout) {
    LoopbackServer server(standardHandler(nullptr));
    auto policy = loopbackPolicy();
    policy.totalTimeout = std::chrono::milliseconds(250);
    NetworkClient client(policy, std::make_shared<CurlTransport>(), onGate());
    const auto start = std::chrono::steady_clock::now();
    auto r = client.fetch(get(server.url("/slow")));
    CHECK(!r.has_value());
    if (failureOf(r.error()) != NetFailure::Timeout) std::fprintf(stderr, "    got %s\n", describe(r.error()).c_str());
    CHECK(failureOf(r.error()) == NetFailure::Timeout);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

RIVET_TEST(netCurlCancellation) {
    LoopbackServer server(standardHandler(nullptr));
    NetworkClient client(loopbackPolicy(), std::make_shared<CurlTransport>(), onGate());
    CancelToken cancel;
    std::thread canceller([cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancel.cancel();
    });
    const auto start = std::chrono::steady_clock::now();
    auto r = client.fetch(get(server.url("/slow")), cancel);
    canceller.join();
    CHECK(!r.has_value());
    if (failureOf(r.error()) != NetFailure::Cancelled) std::fprintf(stderr, "    got %s\n", describe(r.error()).c_str());
    CHECK(failureOf(r.error()) == NetFailure::Cancelled);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

RIVET_TEST(netCurlConnectionRefusedIsTransportError) {
    // Port of a just-closed listener: nothing is listening.
    int port = 0;
    {
        LoopbackServer server(standardHandler(nullptr));
        port = server.port();
    }
    NetworkClient client(loopbackPolicy(), std::make_shared<CurlTransport>(), onGate());
    auto r = client.fetch(get("http://127.0.0.1:" + std::to_string(port) + "/"));
    CHECK(!r.has_value());
    CHECK(failureOf(r.error()) == NetFailure::Transport);
}

#else

RIVET_TEST(netCurlTestsSkipped) {
#ifdef RIVET_NET_CURL
    std::fprintf(stderr, "    (libcurl loopback tests are POSIX only; skipped)\n");
#else
    std::fprintf(stderr, "    (libcurl backend not built; skipped)\n");
    CHECK(!rivet::net::networkBackendAvailable());
#endif
}

#endif
