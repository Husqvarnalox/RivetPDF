// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "LinuxXSettings.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The XSETTINGS byte parser behind the Linux dark-theme detection (pure: the
// backend file has no X11 dependency and is compiled into this test).

using rivet::platform::themeNameIsDark;
using rivet::platform::xsettingsString;

namespace {

struct Builder {
    bool msb = false;
    std::vector<std::uint8_t> bytes;

    void card(std::uint32_t value, int size) {
        for (int i = 0; i < size; ++i) {
            const int shift = msb ? 8 * (size - 1 - i) : 8 * i;
            bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }
    void pad() {
        while (bytes.size() % 4 != 0) bytes.push_back(0);
    }
    void text(std::string_view s) {
        bytes.insert(bytes.end(), s.begin(), s.end());
        pad();
    }
    void header(std::uint32_t count) {
        bytes.push_back(msb ? 1 : 0);
        bytes.push_back(0);
        bytes.push_back(0);
        bytes.push_back(0);
        card(7, 4); // serial
        card(count, 4);
    }
    void intSetting(std::string_view name, std::uint32_t value) {
        card(0, 1);
        bytes.push_back(0);
        card(static_cast<std::uint32_t>(name.size()), 2);
        text(name);
        card(0, 4);
        card(value, 4);
    }
    void stringSetting(std::string_view name, std::string_view value) {
        card(1, 1);
        bytes.push_back(0);
        card(static_cast<std::uint32_t>(name.size()), 2);
        text(name);
        card(0, 4);
        card(static_cast<std::uint32_t>(value.size()), 4);
        text(value);
    }
    void colorSetting(std::string_view name) {
        card(2, 1);
        bytes.push_back(0);
        card(static_cast<std::uint32_t>(name.size()), 2);
        text(name);
        card(0, 4);
        card(0xFFFF, 2);
        card(0, 2);
        card(0, 2);
        card(0xFFFF, 2);
    }
};

} // namespace

RIVET_TEST(xsettingsFindsThemeNameInBothByteOrders) {
    for (const bool msb : {false, true}) {
        Builder b;
        b.msb = msb;
        b.header(3);
        b.intSetting("Net/DoubleClickTime", 400);
        b.colorSetting("Gtk/Color");
        b.stringSetting("Net/ThemeName", "Adwaita-dark");
        const std::optional<std::string> theme = xsettingsString(b.bytes, "Net/ThemeName");
        CHECK(theme.has_value());
        if (theme) CHECK_EQ(*theme, std::string("Adwaita-dark"));
        CHECK(!xsettingsString(b.bytes, "Net/IconThemeName").has_value());
    }
}

RIVET_TEST(xsettingsRejectsTruncatedData) {
    Builder b;
    b.header(1);
    b.stringSetting("Net/ThemeName", "Yaru");
    for (std::size_t size = 0; size < b.bytes.size(); ++size) {
        const std::vector<std::uint8_t> cut(b.bytes.begin(), b.bytes.begin() + static_cast<std::ptrdiff_t>(size));
        CHECK(!xsettingsString(cut, "Net/ThemeName").has_value());
    }
    CHECK(xsettingsString(b.bytes, "Net/ThemeName") == std::optional<std::string>("Yaru"));
}

RIVET_TEST(themeNamesContainingDarkAreDark) {
    CHECK(themeNameIsDark("Adwaita-dark"));
    CHECK(themeNameIsDark("Breeze Dark"));
    CHECK(themeNameIsDark("Yaru-DARK"));
    CHECK(!themeNameIsDark("Adwaita"));
    CHECK(!themeNameIsDark(""));
}
