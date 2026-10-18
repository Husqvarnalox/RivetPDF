// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "net/Url.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rivet::net {

enum class HttpMethod : std::uint8_t { Get, Post };

using HttpHeader = std::pair<std::string, std::string>;
using HttpHeaders = std::vector<HttpHeader>;

// Per-request limits. Zero / empty means "use the NetworkPolicy value"; a
// request can only tighten the policy, never loosen it.
struct HttpTimeouts {
    std::chrono::milliseconds connect{0};
    std::chrono::milliseconds total{0};
};

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    Url url;
    HttpHeaders headers;     // validated by NetworkClient; see forbidden header list
    std::string body;        // raw bytes, POST only
    HttpTimeouts timeouts;
    std::optional<std::size_t> maxResponseBytes;
    std::optional<int> maxRedirects;
};

struct HttpResponse {
    int status = 0;
    HttpHeaders headers;
    std::string body;        // raw bytes
    Url finalUrl;            // URL of the last hop
};

// Case-insensitive header lookup (first match).
std::optional<std::string_view> findHeader(const HttpHeaders& headers, std::string_view name);

} // namespace rivet::net
