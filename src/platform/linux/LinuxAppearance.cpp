// SPDX-License-Identifier: MPL-2.0
#include "LinuxAppearance.hpp"

#include "LinuxXSettings.hpp"

#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include "X11Includes.hpp" // last: Xlib macros

namespace rivet::platform {
namespace {

std::optional<bool> fromGtkThemeVariable() {
    const char* value = std::getenv("GTK_THEME"); // NOLINT(concurrency-mt-unsafe): main thread
    if (value == nullptr) return std::nullopt;
    const std::string_view text{value};
    if (text.ends_with(":dark")) return true;
    return themeNameIsDark(text) ? std::optional<bool>(true) : std::nullopt;
}

std::optional<bool> fromXSettings(Display* display) {
    if (display == nullptr) return std::nullopt;
    const std::string selectionName = "_XSETTINGS_S" + std::to_string(DefaultScreen(display));
    const Atom selection = XInternAtom(display, selectionName.c_str(), False);
    const Atom property = XInternAtom(display, "_XSETTINGS_SETTINGS", False);
    const Window owner = XGetSelectionOwner(display, selection);
    if (owner == x11::kNone) return std::nullopt;

    Atom type = x11::kNone;
    int format = 0;
    unsigned long items = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(display, owner, property, 0, 1L << 16, False, property, &type, &format,
                                          &items, &remaining, &data);
    std::optional<bool> result;
    if (status == 0 && data != nullptr && format == 8) {
        const std::optional<std::string> theme =
            xsettingsString(std::span<const std::uint8_t>(data, items), "Net/ThemeName");
        if (theme) result = themeNameIsDark(*theme);
    }
    if (data != nullptr) XFree(data);
    return result;
}

} // namespace

LinuxAppearance::LinuxAppearance(_XDisplay* display) {
    if (const std::optional<bool> forced = themeOverrideFromEnvironment(); forced.has_value()) {
        dark_ = *forced;
    } else if (const std::optional<bool> gtk = fromGtkThemeVariable(); gtk.has_value()) {
        dark_ = *gtk;
    } else {
        dark_ = fromXSettings(display).value_or(false);
    }
}

} // namespace rivet::platform
