// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/Appearance.hpp"

#include <functional>
#include <optional>

struct _XDisplay;

namespace rivet::platform {

// Light/dark for the X11 shell (ADR-0033 §2), resolved once at startup:
// RIVET_THEME=light|dark, then GTK_THEME ending in ":dark", then the
// XSETTINGS Net/ThemeName (read from the _XSETTINGS_S<screen> owner)
// containing "dark", else light. There is no portable accent: the theme's
// default blue. The setting is not watched (no XSETTINGS change tracking),
// so the change callback never fires; a restart picks up a new theme.
// Main thread only.
class LinuxAppearance final : public IAppearance {
public:
    explicit LinuxAppearance(_XDisplay* display);

    bool isDark() const override { return dark_; }
    std::optional<AccentColor> accentColor() const override { return std::nullopt; }
    void onChange(std::function<void()> callback) override { callback_ = std::move(callback); }

private:
    bool dark_ = false;
    std::function<void()> callback_;
};

} // namespace rivet::platform
