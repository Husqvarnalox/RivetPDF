// SPDX-License-Identifier: MPL-2.0
#include "WindowsAppearance.hpp"

#include "WinCommon.hpp" // last: <windows.h> macros

#include <dwmapi.h>

#include <utility>

namespace rivet::platform {

WindowsAppearance::WindowsAppearance(core::IMainThreadDispatcher& dispatcher)
    : dispatcher_(dispatcher), state_(std::make_shared<State>()) {}

WindowsAppearance::~WindowsAppearance() { state_->callback = nullptr; }

bool WindowsAppearance::systemPrefersDark() {
    if (const std::optional<bool> forced = themeOverrideFromEnvironment(); forced.has_value()) return *forced;
    DWORD value = 1;
    DWORD size = sizeof value;
    const LSTATUS status =
        ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                       L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value == 0;
}

bool WindowsAppearance::isDark() const { return systemPrefersDark(); }

std::optional<AccentColor> WindowsAppearance::accentColor() const {
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (FAILED(::DwmGetColorizationColor(&color, &opaque))) return std::nullopt;
    // 0xAARRGGBB.
    return AccentColor{static_cast<double>((color >> 16U) & 0xFFU) / 255.0,
                       static_cast<double>((color >> 8U) & 0xFFU) / 255.0, static_cast<double>(color & 0xFFU) / 255.0};
}

void WindowsAppearance::onChange(std::function<void()> callback) { state_->callback = std::move(callback); }

void WindowsAppearance::notifyChanged() {
    std::weak_ptr<State> weak = state_;
    dispatcher_.post([weak] {
        const auto alive = weak.lock();
        if (!alive || !alive->callback) return;
        const std::function<void()> callback = alive->callback;
        callback();
    });
}

} // namespace rivet::platform
