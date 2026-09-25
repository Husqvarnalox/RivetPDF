// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

namespace rivet::app {

// Scheme allow-list for external links (PDF is untrusted input). Only these
// schemes are ever opened, and only after an explicit user click; a future
// security review is required before extending the list. file:, javascript:,
// shell: and custom executable schemes are rejected here.
inline bool isAllowedExternalUrlScheme(std::string_view url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0 ||
           url.rfind("mailto:", 0) == 0;
}

} // namespace rivet::app
