// SPDX-License-Identifier: MPL-2.0
#include "LinuxClipboard.hpp"

#include "X11Window.hpp"

namespace rivet::platform {

namespace {

bool isValidUtf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        if (b0 < 0x80) {
            length = 1;
        } else if ((b0 & 0xE0) == 0xC0 && b0 >= 0xC2) {
            length = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            length = 3;
        } else if ((b0 & 0xF8) == 0xF0 && b0 <= 0xF4) {
            length = 4;
        } else {
            return false;
        }
        if (i + length > text.size()) return false;
        for (std::size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        }
        i += length;
    }
    return true;
}

} // namespace

core::Status LinuxClipboard::setText(const std::string& text) {
    // Same contract as the NSPasteboard backend: invalid UTF-8 is an
    // argument error rather than stored garbage.
    if (!isValidUtf8(text)) {
        return std::unexpected(
            core::makeError(core::ErrorCode::InvalidArgument, "clipboard text is not valid UTF-8", "platform"));
    }
    if (!connection_.setClipboardText(text)) {
        return std::unexpected(
            core::makeError(core::ErrorCode::Internal, "the X server refused the clipboard selection", "platform"));
    }
    return core::ok();
}

std::string LinuxClipboard::text() const {
    return connection_.clipboardText();
}

} // namespace rivet::platform
