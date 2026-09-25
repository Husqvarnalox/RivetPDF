// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"

#include <string>

namespace rivet::platform {

// Platform-independent clipboard. Phase 2 needs text only (copy selected
// text); images and rich formats are future work. Implemented per platform
// (NSPasteboard on macOS) inside the platform layer.
class IClipboard {
public:
    virtual ~IClipboard() = default;

    // Replaces the clipboard content with the given UTF-8 text.
    virtual core::Status setText(const std::string& text) = 0;

    // Current clipboard text; empty when the clipboard holds no text.
    virtual std::string text() const = 0;
};

} // namespace rivet::platform
