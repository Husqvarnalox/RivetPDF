// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/async/IMainThreadDispatcher.hpp"
#include "platform/Appearance.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace rivet::platform {

// NSApp.effectiveAppearance (dark aqua or not) and NSColor.controlAccentColor
// (ADR-0033 §2). Changes are observed through KVO on effectiveAppearance and
// NSSystemColorsDidChangeNotification and delivered on the main thread via
// the dispatcher. RIVET_THEME=light|dark forces the appearance: NSApp's
// appearance is then set to aqua / darkAqua so the window chrome follows.
// Main thread only (construct after NSApplication exists).
class MacosAppearance final : public IAppearance {
public:
    explicit MacosAppearance(core::IMainThreadDispatcher& dispatcher);
    ~MacosAppearance() override;

    MacosAppearance(const MacosAppearance&) = delete;
    MacosAppearance& operator=(const MacosAppearance&) = delete;

    bool isDark() const override;
    std::optional<AccentColor> accentColor() const override;
    void onChange(std::function<void()> callback) override;

    struct State; // shared with the ObjC observer (callback slot)

private:
    std::shared_ptr<State> state_;
    void* observer_ = nullptr; // RivetAppearanceObserver*, retained (bridged)
};

} // namespace rivet::platform
