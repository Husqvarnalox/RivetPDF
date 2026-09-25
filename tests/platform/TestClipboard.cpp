// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "platform/macos/MacosClipboard.h"

#include <string>

// macOS pasteboard round-trip. Runs on the main thread of the test process
// (NSPasteboard usage), no GUI needed.
RIVET_TEST(clipboardRoundTrip) {
    rivet::platform::MacosClipboard clipboard;

    const rivet::core::Status written = clipboard.setText("Hello Rivet — Привет");
    CHECK(written.has_value());
    CHECK_EQ(clipboard.text(), std::string("Hello Rivet — Привет"));

    // Overwrite.
    CHECK(clipboard.setText("second").has_value());
    CHECK_EQ(clipboard.text(), std::string("second"));

    // Invalid UTF-8 is rejected, not stored.
    std::string invalid = "ok";
    invalid.push_back(static_cast<char>(0xFF));
    const rivet::core::Status bad = clipboard.setText(invalid);
    CHECK(!bad.has_value());
}
