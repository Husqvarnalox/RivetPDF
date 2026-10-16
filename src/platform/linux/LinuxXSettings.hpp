// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rivet::platform {

// Parser for the XSETTINGS wire format (the _XSETTINGS_SETTINGS property of
// the _XSETTINGS_S<n> selection owner; freedesktop XSETTINGS spec 0.5). Pure
// byte parsing, no X11: unit-tested on every UNIX build. Returns the string
// value of `name` (e.g. "Net/ThemeName"), nullopt when absent or malformed.
std::optional<std::string> xsettingsString(std::span<const std::uint8_t> data, std::string_view name);

// Whether a GTK/desktop theme name denotes a dark theme ("Adwaita-dark",
// "Yaru-dark", "Breeze Dark"): the name contains "dark", any case.
bool themeNameIsDark(std::string_view themeName);

} // namespace rivet::platform
