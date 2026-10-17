// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdlib>
#include <functional>
#include <optional>
#include <string_view>

namespace rivet::platform {

// An sRGB color with channels 0..1 (the platform layer does not depend on
// rivet::ui; the shell converts it into a theme accent).
struct AccentColor {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;

    friend bool operator==(const AccentColor&, const AccentColor&) = default;
};

// The system appearance the shell themes itself after (ADR-0033 §2):
//   - macOS:   NSApp.effectiveAppearance (dark aqua) + NSColor.controlAccentColor
//   - Windows: AppsUseLightTheme registry value + DwmGetColorizationColor,
//              re-read on WM_SETTINGCHANGE
//   - Linux:   RIVET_THEME, then the XSETTINGS Net/ThemeName containing
//              "dark", else light; default accent
// Every backend honors RIVET_THEME=light|dark first (debugging, screenshots).
//
// Main thread only. The change callback is delivered on the main thread
// (never under a lock); the shell re-reads isDark()/accentColor() in it.
class IAppearance {
public:
    virtual ~IAppearance() = default;

    virtual bool isDark() const = 0;
    // nullopt = no platform accent (the theme's default blue).
    virtual std::optional<AccentColor> accentColor() const = 0;
    // Replaces the single change observer; an empty function clears it. The
    // owner must clear it before the objects the callback captures die.
    virtual void onChange(std::function<void()> callback) = 0;
};

// RIVET_THEME=light|dark (case-sensitive), shared by every backend.
inline std::optional<bool> themeOverrideFromEnvironment() {
    const char* value = std::getenv("RIVET_THEME"); // NOLINT(concurrency-mt-unsafe): main thread
    if (value == nullptr) return std::nullopt;
    const std::string_view text{value};
    if (text == "dark") return true;
    if (text == "light") return false;
    return std::nullopt;
}

} // namespace rivet::platform
