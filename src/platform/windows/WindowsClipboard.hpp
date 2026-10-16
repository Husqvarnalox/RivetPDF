// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/Clipboard.hpp"

struct HWND__;

namespace rivet::platform {

// System clipboard through the Win32 clipboard (CF_UNICODETEXT). Writing
// turns bare LF line breaks into CRLF (the Windows text convention); reading
// turns CRLF back into LF. Main thread only.
class WindowsClipboard final : public IClipboard {
public:
    // `owner` is the clipboard owner window (may be null).
    explicit WindowsClipboard(HWND__* owner) : owner_(owner) {}

    void setOwner(HWND__* owner) { owner_ = owner; }

    core::Status setText(const std::string& text) override;
    std::string text() const override;

private:
    HWND__* owner_ = nullptr;
};

} // namespace rivet::platform
