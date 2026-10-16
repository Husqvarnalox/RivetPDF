// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/ExternalUrlOpener.hpp"

namespace rivet::platform {

// Hands an already-validated URL to `xdg-open` (posix_spawn, no shell; the
// URL is one argv entry, verbatim). Re-checks the same well-formedness rules
// as the macOS opener and additionally refuses a leading '-' so the URL can
// never be read as an option.
class LinuxExternalUrlOpener final : public IExternalUrlOpener {
public:
    core::Status openUrl(const std::string& url) override;
};

} // namespace rivet::platform
