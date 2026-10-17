// SPDX-License-Identifier: MPL-2.0
#include "net/Url.hpp"

#include "net/Error.hpp"

#include <cstddef>
#include <vector>

namespace rivet::net {

namespace {

core::Result<Url> bad(std::string message) { return netFail<Url>(NetFailure::InvalidRequest, std::move(message)); }

bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool hasBadByte(std::string_view s) {
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7F) return true; // control, space, DEL, non-ASCII
        if (c == '\\') return true;
    }
    return false;
}

// "ends in a number": the last label is all digits or 0x-hex, so the host
// must be an IPv4 literal (WHATWG rule) and cannot be treated as a name.
bool endsInNumber(std::string_view host) {
    const std::size_t dot = host.rfind('.');
    const std::string_view last = dot == std::string_view::npos ? host : host.substr(dot + 1);
    if (last.empty()) return false;
    bool allDigits = true;
    for (char c : last)
        if (!isDigit(c)) allDigits = false;
    if (allDigits) return true;
    if (last.size() >= 2 && last[0] == '0' && (last[1] == 'x' || last[1] == 'X')) {
        for (std::size_t i = 2; i < last.size(); ++i) {
            const char c = last[i];
            if (!(isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        }
        return true;
    }
    return false;
}

bool validDnsName(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    std::size_t labelLen = 0;
    char prev = '.';
    for (char c : host) {
        if (c == '.') {
            if (labelLen == 0 || prev == '-') return false;
            labelLen = 0;
        } else {
            const bool ok = isAlpha(c) || isDigit(c) || c == '-';
            if (!ok) return false;
            if (c == '-' && labelLen == 0) return false;
            if (++labelLen > 63) return false;
        }
        prev = c;
    }
    return labelLen != 0 && prev != '-';
}

} // namespace

std::string Url::origin() const {
    std::string out = scheme + "://";
    out += (ip && ip->isV6) ? "[" + host + "]" : host;
    const std::uint16_t defaultPort = isHttps() ? 443 : 80;
    if (port != defaultPort) out += ":" + std::to_string(port);
    return out;
}

std::string Url::toString() const { return origin() + target; }

core::Result<Url> parseUrl(std::string_view text) {
    if (text.empty()) return bad("empty URL");
    if (text.size() > kMaxUrlLength) return bad("URL too long");
    if (hasBadByte(text)) return bad("URL contains whitespace, control, non-ASCII or backslash characters");

    const std::size_t hash = text.find('#');
    if (hash != std::string_view::npos) text = text.substr(0, hash);

    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0) return bad("missing URL scheme");
    std::string scheme;
    for (std::size_t i = 0; i < colon; ++i) {
        if (!(isAlpha(text[i]) || (i > 0 && (isDigit(text[i]) || text[i] == '+' || text[i] == '-' || text[i] == '.'))))
            return bad("invalid URL scheme");
        scheme += lower(text[i]);
    }
    if (scheme != "http" && scheme != "https") return bad("unsupported URL scheme '" + scheme + "'");
    if (text.substr(colon, 3) != "://") return bad("URL must have the form scheme://host/");
    std::string_view rest = text.substr(colon + 3);

    const std::size_t authEnd = rest.find_first_of("/?");
    const std::string_view authority = rest.substr(0, authEnd);
    std::string target = authEnd == std::string_view::npos ? "/" : std::string(rest.substr(authEnd));
    if (target[0] == '?') target.insert(target.begin(), '/');

    if (authority.empty()) return bad("empty host");
    if (authority.find('@') != std::string_view::npos) return bad("userinfo in URL is not allowed");

    Url url;
    url.scheme = scheme;
    url.target = std::move(target);

    std::string_view hostPart = authority;
    std::string_view portPart;
    bool havePort = false;
    if (authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return bad("unterminated IPv6 literal");
        hostPart = authority.substr(1, close - 1);
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') return bad("garbage after IPv6 literal");
            portPart = after.substr(1);
            havePort = true;
        }
        const auto ip = parseIpv6(hostPart);
        if (!ip) return bad("invalid IPv6 literal");
        url.ip = *ip;
        url.host = formatIp(*ip);
    } else {
        const std::size_t c = authority.rfind(':');
        if (c != std::string_view::npos) {
            hostPart = authority.substr(0, c);
            portPart = authority.substr(c + 1);
            havePort = true;
        }
        if (hostPart.empty()) return bad("empty host");
        std::string host;
        for (char ch : hostPart) host += lower(ch);
        if (host.size() > 1 && host.back() == '.') host.pop_back(); // one trailing dot
        if (host.empty() || host.back() == '.') return bad("invalid host");
        if (endsInNumber(host)) {
            const auto ip = parseIpv4Legacy(host);
            if (!ip) return bad("invalid numeric host");
            url.ip = *ip;
            url.host = formatIp(*ip);
        } else {
            if (!validDnsName(host)) return bad("invalid host name (IDN hosts must be punycode)");
            url.host = std::move(host);
        }
    }

    if (havePort) {
        if (portPart.empty() || portPart.size() > 5) return bad("invalid port");
        unsigned value = 0;
        for (char ch : portPart) {
            if (!isDigit(ch)) return bad("invalid port");
            value = value * 10 + static_cast<unsigned>(ch - '0');
        }
        if (value < 1 || value > 65535) return bad("port out of range");
        url.port = static_cast<std::uint16_t>(value);
    } else {
        url.port = scheme == "https" ? 443 : 80;
    }
    return url;
}

namespace {

std::string removeDotSegments(const std::string& path) {
    std::vector<std::string> out;
    std::size_t i = 0;
    bool trailingSlash = false;
    while (i <= path.size()) {
        const std::size_t next = path.find('/', i);
        const std::string seg = path.substr(i, next == std::string::npos ? std::string::npos : next - i);
        const bool last = next == std::string::npos;
        if (seg == "..") {
            if (!out.empty()) out.pop_back();
            trailingSlash = last;
        } else if (seg == ".") {
            trailingSlash = last;
        } else if (!(seg.empty() && i == 0)) {
            out.push_back(seg);
            trailingSlash = false;
        }
        if (last) break;
        i = next + 1;
    }
    std::string result;
    for (const auto& s : out) result += "/" + s;
    if (result.empty()) result = "/";
    else if (trailingSlash && result.back() != '/') result += '/';
    return result;
}

} // namespace

core::Result<Url> resolveReference(const Url& base, std::string_view ref) {
    if (ref.empty()) return bad("empty reference");
    if (ref.size() > kMaxUrlLength) return bad("reference too long");
    if (hasBadByte(ref)) return bad("reference contains forbidden characters");
    const std::size_t hash = ref.find('#');
    if (hash != std::string_view::npos) ref = ref.substr(0, hash);
    if (ref.empty()) return parseUrl(base.toString());

    if (ref.substr(0, 2) == "//") return parseUrl(base.scheme + ":" + std::string(ref));

    // A scheme prefix (letters then ':' before any '/', '?') means absolute.
    const std::size_t stop = ref.find_first_of("/?");
    const std::size_t colon = ref.find(':');
    if (colon != std::string_view::npos && (stop == std::string_view::npos || colon < stop)) return parseUrl(ref);

    const std::size_t q = base.target.find('?');
    const std::string basePath = base.target.substr(0, q);
    std::string target;
    if (ref[0] == '/') {
        target = std::string(ref);
    } else if (ref[0] == '?') {
        target = basePath + std::string(ref);
    } else {
        const std::size_t slash = basePath.rfind('/');
        target = basePath.substr(0, slash + 1) + std::string(ref);
    }
    const std::size_t tq = target.find('?');
    const std::string path = removeDotSegments(target.substr(0, tq));
    target = path + (tq == std::string::npos ? std::string() : target.substr(tq));
    return parseUrl(base.origin() + target);
}

} // namespace rivet::net
