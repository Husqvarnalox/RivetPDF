// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/UrlPolicy.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace rivet::app {

// What a click on a Markdown link means. Markdown is untrusted input: the
// decision is made from the URL text alone and nothing is ever executed.
enum class MarkdownLinkKind {
    Empty,    // no target
    External, // http / https / mailto (UrlPolicy): handed to the URL opener
    Anchor,   // "#slug": scroll to the heading in the same document
    Local,    // relative or absolute file path: NOT opened (documented), only reported
    Unsafe,   // any other scheme (file:, javascript:, data:, custom) or control characters
};

struct MarkdownLinkDecision {
    MarkdownLinkKind kind = MarkdownLinkKind::Empty;
    std::string target; // External: the URL; Anchor: the percent-decoded slug without '#'
};

namespace detail {

inline int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace detail

// Decodes %XX escapes (invalid escapes stay literal). Used for anchors and
// local image paths.
inline std::string percentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && detail::hexValue(text[i + 1]) >= 0 &&
            detail::hexValue(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(detail::hexValue(text[i + 1]) * 16 + detail::hexValue(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

// True when `url` starts with a URI scheme ("name:" before any '/', '?' or
// '#'). A single letter before ':' is a Windows drive ("C:\x"), not a scheme.
inline bool hasUriScheme(std::string_view url) {
    if (url.empty() || !std::isalpha(static_cast<unsigned char>(url.front()))) return false;
    for (std::size_t i = 1; i < url.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(url[i]);
        if (c == ':') return i >= 2;
        if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.')) return false;
    }
    return false;
}

inline MarkdownLinkDecision classifyMarkdownLink(std::string_view url) {
    MarkdownLinkDecision decision;
    if (url.empty()) return decision;
    for (const char ch : url) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f) {
            decision.kind = MarkdownLinkKind::Unsafe;
            return decision;
        }
    }
    if (url.front() == '#') {
        decision.kind = MarkdownLinkKind::Anchor;
        decision.target = percentDecode(url.substr(1));
        return decision;
    }
    if (isAllowedExternalUrlScheme(url)) {
        decision.kind = MarkdownLinkKind::External;
        decision.target = std::string(url);
        return decision;
    }
    if (hasUriScheme(url) || url.rfind("//", 0) == 0) {
        decision.kind = MarkdownLinkKind::Unsafe;
        return decision;
    }
    decision.kind = MarkdownLinkKind::Local;
    decision.target = std::string(url);
    return decision;
}

} // namespace rivet::app
