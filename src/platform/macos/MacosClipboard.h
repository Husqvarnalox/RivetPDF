// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/Clipboard.hpp"

namespace rivet::platform {

// NSPasteboard-backed clipboard. Must be used on the main thread
// (NSPasteboard general pasteboard usage is main-thread in practice).
class MacosClipboard final : public IClipboard {
public:
    core::Status setText(const std::string& text) override;
    std::string text() const override;
};

} // namespace rivet::platform
