// SPDX-License-Identifier: MPL-2.0
#include "net/Policy.hpp"

#include "net/Error.hpp"

#include <algorithm>
#include <array>

namespace rivet::net {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view s) {
    std::string out;
    for (char c : s) out += lower(c);
    return out;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

core::Status denied(NetFailure f, std::string m) { return std::unexpected(makeNetError(f, std::move(m))); }

bool isTokenChar(char c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

} // namespace

bool hostMatchesPattern(std::string_view pattern, std::string_view host) {
    const std::string p = lowered(pattern);
    const std::string h = lowered(host);
    if (p.size() > 2 && p[0] == '*' && p[1] == '.') {
        const std::string suffix = p.substr(1); // ".example.com"
        return h.size() > suffix.size() && endsWith(h, suffix);
    }
    return !p.empty() && p == h;
}

bool hostAllowed(const NetworkPolicy& policy, std::string_view host) {
    if (policy.allowedHosts.empty()) return true;
    return std::any_of(policy.allowedHosts.begin(), policy.allowedHosts.end(),
                       [&](const std::string& p) { return hostMatchesPattern(p, host); });
}

bool isPrivateHostName(std::string_view hostIn) {
    const std::string host = lowered(hostIn);
    if (host == "localhost") return true;
    for (std::string_view suffix : {".localhost", ".local", ".internal"})
        if (endsWith(host, suffix)) return true;
    return false;
}

core::Status checkScheme(const Url& url, const NetworkPolicy& policy) {
    if (url.scheme == "https") return core::ok();
    if (url.scheme == "http") {
        if (policy.allowHttp) return core::ok();
        return denied(NetFailure::PolicyViolation, "plain http is not allowed (https only)");
    }
    return denied(NetFailure::PolicyViolation, "unsupported URL scheme");
}

core::Status checkAllowList(const Url& url, const NetworkPolicy& policy) {
    if (hostAllowed(policy, url.host)) return core::ok();
    return denied(NetFailure::PolicyViolation, "host '" + url.host + "' is not in the allowed host list");
}

core::Status checkHostAddress(const Url& url, const NetworkPolicy& policy) {
    if (policy.allowPrivateNetworks) return core::ok();
    if (url.ip) {
        if (isForbiddenAddress(*url.ip))
            return denied(NetFailure::ForbiddenAddress, "address " + url.host + " is in a forbidden range");
        return core::ok();
    }
    if (isPrivateHostName(url.host))
        return denied(NetFailure::PolicyViolation, "host name '" + url.host + "' refers to a private network");
    return core::ok();
}

core::Status checkProxyDestination(const Url& url, const NetworkPolicy& policy) {
    if (!policy.useSystemProxy || policy.allowPrivateNetworks || url.ip) return core::ok();
    if (policy.allowedHosts.empty())
        return denied(NetFailure::PolicyViolation,
                      "a system proxy resolves host names itself: an allowed-host list is required to use one");
    return core::ok(); // checkAllowList has already matched url.host against the list
}

core::Status checkUrl(const Url& url, const NetworkPolicy& policy) {
    if (auto s = checkScheme(url, policy); !s) return s;
    if (auto s = checkAllowList(url, policy); !s) return s;
    if (auto s = checkHostAddress(url, policy); !s) return s;
    return checkProxyDestination(url, policy);
}

EffectiveLimits effectiveLimits(const HttpRequest& request, const NetworkPolicy& policy) {
    EffectiveLimits out{policy.maxResponseBytes, policy.connectTimeout, policy.totalTimeout};
    if (request.maxResponseBytes) out.maxResponseBytes = std::min(out.maxResponseBytes, *request.maxResponseBytes);
    if (request.timeouts.connect.count() > 0) out.connectTimeout = std::min(out.connectTimeout, request.timeouts.connect);
    if (request.timeouts.total.count() > 0) out.totalTimeout = std::min(out.totalTimeout, request.timeouts.total);
    out.connectTimeout = std::min(out.connectTimeout, out.totalTimeout);
    return out;
}

core::Status validateHeaders(const HttpHeaders& headers) {
    static constexpr std::array<std::string_view, 11> kForbidden{
        "host", "user-agent", "cookie", "authorization", "proxy-authorization", "referer",
        "content-length", "transfer-encoding", "connection", "upgrade", "expect"};
    for (const auto& [name, value] : headers) {
        if (name.empty() || !std::all_of(name.begin(), name.end(), isTokenChar))
            return denied(NetFailure::InvalidRequest, "invalid header name");
        for (char ch : value) {
            const auto c = static_cast<unsigned char>(ch);
            if ((c < 0x20 && c != '\t') || c == 0x7F) return denied(NetFailure::InvalidRequest, "invalid header value");
        }
        const std::string n = lowered(name);
        if (std::find(kForbidden.begin(), kForbidden.end(), n) != kForbidden.end())
            return denied(NetFailure::InvalidRequest, "header '" + name + "' may not be set by the caller");
    }
    return core::ok();
}

} // namespace rivet::net
