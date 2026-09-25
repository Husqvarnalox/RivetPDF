// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"

#include <string>

namespace rivet::platform {

// Opens an external URL in the system handler (browser / mail client).
//
// SECURITY CONTRACT (enforced by the CALLER, documented here):
//   - Only ever invoked after an explicit user click on a link. Rivet never
//     opens URLs automatically.
//   - The caller validates the scheme against a deliberate allow-list
//     (http, https, mailto) BEFORE calling; implementations must treat the
//     input as untrusted and refuse anything but a well-formed absolute URL.
//   - Implementations must not execute, expand, or shell-interpolate the
//     string: it is handed to the platform's URL opener verbatim.
class IExternalUrlOpener {
public:
    virtual ~IExternalUrlOpener() = default;

    // Opens the URL; an error is reported (never thrown) when the platform
    // refuses it.
    virtual core::Status openUrl(const std::string& url) = 0;
};

} // namespace rivet::platform
