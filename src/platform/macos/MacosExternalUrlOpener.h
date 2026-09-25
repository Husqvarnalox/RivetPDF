// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/ExternalUrlOpener.hpp"

namespace rivet::platform {

// NSWorkspace-backed URL opener. Main thread only. The URL is handed to
// NSWorkspace verbatim after the caller's scheme validation (see the
// interface contract).
class MacosExternalUrlOpener final : public IExternalUrlOpener {
public:
    core::Status openUrl(const std::string& url) override;
};

} // namespace rivet::platform
