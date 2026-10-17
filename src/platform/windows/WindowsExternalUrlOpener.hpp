// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/ExternalUrlOpener.hpp"

namespace rivet::platform {

// Hands a validated absolute URL to the system handler via ShellExecuteW
// ("open" verb). The string is passed verbatim (never through a shell);
// anything but a well-formed scheme without control characters or spaces is
// refused first. Main thread (COM apartment of the caller).
class WindowsExternalUrlOpener final : public IExternalUrlOpener {
public:
    core::Status openUrl(const std::string& url) override;
};

} // namespace rivet::platform
