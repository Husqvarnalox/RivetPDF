// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/async/IMainThreadDispatcher.hpp"
#include "platform/Appearance.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace rivet::platform {

// The Windows app theme (HKCU ...\Themes\Personalize AppsUseLightTheme) and
// the DWM colorization color as accent (ADR-0033 §2). The window forwards
// WM_SETTINGCHANGE / WM_THEMECHANGED / WM_DWMCOLORIZATIONCOLORCHANGED to
// notifyChanged(), which delivers the change callback through the main-thread
// dispatcher. RIVET_THEME=light|dark overrides the registry. Main thread only.
class WindowsAppearance final : public IAppearance {
public:
    explicit WindowsAppearance(core::IMainThreadDispatcher& dispatcher);
    ~WindowsAppearance() override;

    WindowsAppearance(const WindowsAppearance&) = delete;
    WindowsAppearance& operator=(const WindowsAppearance&) = delete;

    bool isDark() const override;
    std::optional<AccentColor> accentColor() const override;
    void onChange(std::function<void()> callback) override;

    // A system setting changed (window procedure): posts the callback.
    void notifyChanged();

    // Shared with the window: the dark title bar follows the same rule.
    static bool systemPrefersDark();

private:
    struct State {
        std::function<void()> callback;
    };
    core::IMainThreadDispatcher& dispatcher_;
    std::shared_ptr<State> state_;
};

} // namespace rivet::platform
