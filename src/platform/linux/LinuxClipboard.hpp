// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/Clipboard.hpp"

#include <string>

namespace rivet::platform {

class X11Connection;

// X11 CLIPBOARD selection (UTF8_STRING; STRING fallback). Main thread only:
// it shares the X connection with the event loop. Ownership is served from
// the loop (X11Connection) while Rivet runs; there is no clipboard-manager
// hand-off at exit, and INCR (very large) transfers are not supported.
class LinuxClipboard final : public IClipboard {
public:
    explicit LinuxClipboard(X11Connection& connection) : connection_(connection) {}

    core::Status setText(const std::string& text) override;
    std::string text() const override;

private:
    X11Connection& connection_;
};

} // namespace rivet::platform
