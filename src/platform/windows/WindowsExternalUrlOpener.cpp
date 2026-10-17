// SPDX-License-Identifier: MPL-2.0
#include "WindowsExternalUrlOpener.hpp"

#include "WinCommon.hpp"

#include <shellapi.h>

#include <cctype>

namespace rivet::platform {

core::Status WindowsExternalUrlOpener::openUrl(const std::string& url) {
    const auto invalid = [](const char* message) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, message, "platform"));
    };
    // Second line of defense behind the caller's scheme allow-list (same
    // checks as the X11 backend).
    const auto schemeEnd = url.find("://");
    const bool hasMailto = url.rfind("mailto:", 0) == 0;
    if (!hasMailto && (schemeEnd == std::string::npos || schemeEnd == 0)) {
        return invalid("URL is not an absolute http/https/mailto URL");
    }
    const std::size_t schemeLength = hasMailto ? 6 : schemeEnd;
    for (std::size_t i = 0; i < schemeLength; ++i) {
        const auto c = static_cast<unsigned char>(url[i]);
        if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.') || (i == 0 && !std::isalpha(c))) {
            return invalid("malformed URL scheme");
        }
    }
    for (const char c : url) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x21 || uc == 0x7F) return invalid("URL contains control characters");
    }

    const std::wstring wide = win::widen(url);
    if (wide.empty()) return invalid("URL is not valid UTF-8");
    const auto result = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        return std::unexpected(
            core::makeError(core::ErrorCode::Internal, "the system refused to open the URL", "platform"));
    }
    return core::ok();
}

} // namespace rivet::platform
