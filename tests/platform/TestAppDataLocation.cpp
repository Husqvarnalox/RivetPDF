// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "LinuxAppDataLocation.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

// The XDG rule of the Linux application-data location (pure: the backend
// file is POSIX-only and compiled into this test on every UNIX).

using rivet::platform::LinuxAppDataLocation;
namespace fs = std::filesystem;

RIVET_TEST(linuxAppDataPrefersAbsoluteXdgDataHome) {
    const auto xdg = LinuxAppDataLocation::resolve(std::string_view("/data/me"), std::string_view("/home/me"));
    CHECK(xdg.has_value());
    if (xdg) CHECK(*xdg == fs::path("/data/me/rivet"));

    // Relative or empty XDG_DATA_HOME is invalid per the spec: the home fallback applies.
    const auto relative = LinuxAppDataLocation::resolve(std::string_view("data"), std::string_view("/home/me"));
    CHECK(relative.has_value());
    if (relative) CHECK(*relative == fs::path("/home/me/.local/share/rivet"));
    const auto empty = LinuxAppDataLocation::resolve(std::string_view(""), std::string_view("/home/me"));
    CHECK(empty.has_value());
    if (empty) CHECK(*empty == fs::path("/home/me/.local/share/rivet"));
    const auto unset = LinuxAppDataLocation::resolve(std::nullopt, std::string_view("/home/me"));
    CHECK(unset.has_value());
    if (unset) CHECK(*unset == fs::path("/home/me/.local/share/rivet"));

    CHECK(!LinuxAppDataLocation::resolve(std::nullopt, std::nullopt).has_value());
    CHECK(!LinuxAppDataLocation::resolve(std::nullopt, std::string_view("relative/home")).has_value());

    // The live environment always yields something on a normal account.
    const LinuxAppDataLocation location;
    const auto live = location.directory();
    CHECK(live.has_value());
    if (live) CHECK(live->is_absolute());
}

// RIVET_APP_DATA / RIVET_QA_SCRIPT override shared by every platform
// implementation (scripted QA must not touch the user's Recent list).
RIVET_TEST(appDataOverrideKeepsQaRunsAwayFromUserData) {
    using rivet::platform::resolveAppDataOverride;
    const fs::path temp("/tmp");
    const auto explicitDir = resolveAppDataOverride(std::string_view("/qa/data"), std::nullopt, temp);
    CHECK(explicitDir.has_value());
    if (explicitDir) CHECK(*explicitDir == fs::path("/qa/data"));
    const auto explicitWins = resolveAppDataOverride(std::string_view("/qa/data"), std::string_view("s.rivetqa"), temp);
    CHECK(explicitWins.has_value());
    if (explicitWins) CHECK(*explicitWins == fs::path("/qa/data"));
    const auto qa = resolveAppDataOverride(std::string_view("relative"), std::string_view("s.rivetqa"), temp);
    CHECK(qa.has_value());
    if (qa) CHECK(*qa == fs::path("/tmp/rivet-qa-appdata"));
    CHECK(!resolveAppDataOverride(std::nullopt, std::nullopt, temp).has_value());
    CHECK(!resolveAppDataOverride(std::string_view(""), std::nullopt, temp).has_value());
}
