// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#import "platform/macos/MacosAppDataLocation.h"

#include <filesystem>
#include <string>

// ~/Library/Application Support/Rivet through NSFileManager.

RIVET_TEST(macosAppDataLocationIsApplicationSupportRivet) {
    const rivet::platform::MacosAppDataLocation location;
    const auto directory = location.directory();
    CHECK(directory.has_value());
    if (!directory) return;
    CHECK(directory->is_absolute());
    CHECK_EQ(directory->filename().string(), std::string("Rivet"));
    CHECK_EQ(directory->parent_path().filename().string(), std::string("Application Support"));
}
