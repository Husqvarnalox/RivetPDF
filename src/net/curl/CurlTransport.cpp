// SPDX-License-Identifier: MPL-2.0
#include "net/curl/CurlTransport.hpp"

#include <curl/curl.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace rivet::net {

namespace {

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

struct Context {
    std::size_t maxBytes = 0;
    bool allowPrivate = false;
    CancelToken cancel;
    std::chrono::steady_clock::time_point deadline;

    std::string body;
    HttpHeaders headers;
    std::size_t headerBytes = 0;

    bool tooLarge = false;
    bool headersTooLarge = false;
    bool blocked = false;
    std::string blockedAddress;
    bool cancelled = false;
    bool deadlineHit = false;
};

void globalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t writeCallback(char* data, std::size_t size, std::size_t count, void* user) {
    auto& ctx = *static_cast<Context*>(user);
    const std::size_t length = size * count;
    if (length > ctx.maxBytes || ctx.body.size() > ctx.maxBytes - length) {
        ctx.tooLarge = true;
        return 0; // abort the transfer
    }
    ctx.body.append(data, length);
    return length;
}

std::size_t headerCallback(char* data, std::size_t size, std::size_t count, void* user) {
    auto& ctx = *static_cast<Context*>(user);
    const std::size_t length = size * count;
    ctx.headerBytes += length;
    if (ctx.headerBytes > kMaxHeaderBytes) {
        ctx.headersTooLarge = true;
        return 0;
    }
    std::string_view line(data, length);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (line.substr(0, 5) == "HTTP/") {
        ctx.headers.clear(); // a new status line (1xx, then the final one)
        return length;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return length;
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    ctx.headers.emplace_back(std::string(line.substr(0, colon)), std::string(value));
    return length;
}

int progressCallback(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto& ctx = *static_cast<Context*>(user);
    if (ctx.cancel.cancelled()) {
        ctx.cancelled = true;
        return 1;
    }
    if (std::chrono::steady_clock::now() >= ctx.deadline) {
        ctx.deadlineHit = true;
        return 1;
    }
    return 0;
}

// Called by libcurl for every candidate peer address right before the socket
// is created and connected: the address actually used, after DNS (rebinding)
// and redirects, is what gets classified.
curl_socket_t openSocketCallback(void* user, curlsocktype, struct curl_sockaddr* address) {
    auto& ctx = *static_cast<Context*>(user);
    if (!ctx.allowPrivate) {
        std::optional<IpAddress> peer;
        if (address->family == AF_INET && address->addrlen >= sizeof(sockaddr_in)) {
            sockaddr_in sin{};
            std::memcpy(&sin, &address->addr, sizeof sin);
            const auto* b = reinterpret_cast<const std::uint8_t*>(&sin.sin_addr);
            peer = IpAddress::v4(b[0], b[1], b[2], b[3]);
        } else if (address->family == AF_INET6 && address->addrlen >= sizeof(sockaddr_in6)) {
            sockaddr_in6 sin6{};
            std::memcpy(&sin6, &address->addr, sizeof sin6);
            std::array<std::uint8_t, 16> raw{};
            std::memcpy(raw.data(), &sin6.sin6_addr, 16);
            peer = IpAddress::v6(raw);
        }
        // Unknown address families are refused: fail closed.
        if (!peer || isForbiddenAddress(*peer)) {
            ctx.blocked = true;
            ctx.blockedAddress = peer ? formatIp(*peer) : "unknown address family";
            return CURL_SOCKET_BAD;
        }
    }
    return ::socket(address->family, address->socktype, address->protocol);
}

struct HandleDeleter {
    void operator()(CURL* h) const { curl_easy_cleanup(h); }
};
struct ListDeleter {
    void operator()(curl_slist* l) const { curl_slist_free_all(l); }
};

} // namespace

CurlTransport::CurlTransport() { globalInit(); }

core::Result<HttpResponse> CurlTransport::perform(const HttpRequest& request, const NetworkPolicy& policy,
                                                  CancelToken cancel) {
    // Defence in depth: the client already checked these; the address policy
    // is enforced at connect time below.
    if (auto s = checkScheme(request.url, policy); !s) return std::unexpected(s.error());
    if (auto s = checkAllowList(request.url, policy); !s) return std::unexpected(s.error());
    if (auto s = validateHeaders(request.headers); !s) return std::unexpected(s.error());
    if (request.method == HttpMethod::Get && !request.body.empty())
        return netFail<HttpResponse>(NetFailure::InvalidRequest, "GET requests cannot have a body");
    if (cancel.cancelled()) return netFail<HttpResponse>(NetFailure::Cancelled, "operation cancelled");

    const EffectiveLimits limits = effectiveLimits(request, policy);

    Context ctx;
    ctx.maxBytes = limits.maxResponseBytes;
    ctx.allowPrivate = policy.allowPrivateNetworks;
    ctx.cancel = cancel;
    ctx.deadline = std::chrono::steady_clock::now() + limits.totalTimeout;

    std::unique_ptr<CURL, HandleDeleter> handle(curl_easy_init());
    if (!handle) return netFail<HttpResponse>(NetFailure::Transport, "could not initialise libcurl");
    CURL* h = handle.get();

    CURLcode setup = CURLE_OK;
    auto set = [&](CURLoption option, auto value) {
        const CURLcode rc = curl_easy_setopt(h, option, value);
        if (rc != CURLE_OK && setup == CURLE_OK) setup = rc;
    };

    const std::string url = request.url.toString();
    set(CURLOPT_URL, url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500 // 7.85.0
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    set(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    set(CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    set(CURLOPT_FOLLOWLOCATION, 0L); // hops are driven (and re-validated) by NetworkClient
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    set(CURLOPT_AUTOREFERER, 0L);
    set(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE));
    set(CURLOPT_FAILONERROR, 0L); // 4xx/5xx are ordinary responses
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2)); // minimum
    if (!policy.useSystemProxy) set(CURLOPT_PROXY, "");
    set(CURLOPT_USERAGENT, policy.userAgent.c_str());
    set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(limits.connectTimeout.count()));
    set(CURLOPT_TIMEOUT_MS, static_cast<long>(limits.totalTimeout.count()));
    set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(limits.maxResponseBytes));
    set(CURLOPT_MAXREDIRS, 0L);

    set(CURLOPT_WRITEFUNCTION, &writeCallback);
    set(CURLOPT_WRITEDATA, &ctx);
    set(CURLOPT_HEADERFUNCTION, &headerCallback);
    set(CURLOPT_HEADERDATA, &ctx);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, &progressCallback);
    set(CURLOPT_XFERINFODATA, &ctx);
    set(CURLOPT_OPENSOCKETFUNCTION, &openSocketCallback);
    set(CURLOPT_OPENSOCKETDATA, &ctx);

    std::unique_ptr<curl_slist, ListDeleter> headerList;
    for (const auto& [name, value] : request.headers) {
        const std::string line = name + ": " + value;
        curl_slist* next = curl_slist_append(headerList.get(), line.c_str());
        if (!next) return netFail<HttpResponse>(NetFailure::Transport, "out of memory");
        headerList.release();
        headerList.reset(next);
    }
    if (request.method == HttpMethod::Post) {
        set(CURLOPT_POST, 1L);
        set(CURLOPT_POSTFIELDS, request.body.data());
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    } else {
        set(CURLOPT_HTTPGET, 1L);
    }
    if (headerList) set(CURLOPT_HTTPHEADER, headerList.get());

    if (setup != CURLE_OK)
        return netFail<HttpResponse>(NetFailure::Transport,
                                     std::string("libcurl option failed: ") + curl_easy_strerror(setup));

    const CURLcode rc = curl_easy_perform(h);

    // Our own abort reasons first: they surface as generic curl errors.
    if (ctx.blocked)
        return netFail<HttpResponse>(NetFailure::ForbiddenAddress,
                                     "refusing to connect to forbidden address " + ctx.blockedAddress);
    if (ctx.cancelled || cancel.cancelled()) return netFail<HttpResponse>(NetFailure::Cancelled, "operation cancelled");
    if (ctx.tooLarge || rc == CURLE_FILESIZE_EXCEEDED)
        return netFail<HttpResponse>(NetFailure::ResponseTooLarge, "response exceeds the size limit");
    if (ctx.headersTooLarge) return netFail<HttpResponse>(NetFailure::ResponseTooLarge, "response headers too large");
    if (ctx.deadlineHit || rc == CURLE_OPERATION_TIMEDOUT)
        return netFail<HttpResponse>(NetFailure::Timeout, "request timed out");
    if (rc != CURLE_OK)
        return netFail<HttpResponse>(NetFailure::Transport, std::string("transfer failed: ") + curl_easy_strerror(rc));

    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);

    HttpResponse response;
    response.status = static_cast<int>(status);
    response.headers = std::move(ctx.headers);
    response.body = std::move(ctx.body);
    response.finalUrl = request.url;
    return response;
}

} // namespace rivet::net
