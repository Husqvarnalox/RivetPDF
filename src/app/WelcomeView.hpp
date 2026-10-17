// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/Widget.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace rivet::ui {
class Button;
}

namespace rivet::app {

// The calm empty-workspace view (ADR-0033 §5): the app name, a primary
// "Open…" button, the recent files (missing ones greyed and inert) and a
// drag-and-drop hint, centered in the content area. Main thread only.
class WelcomeView final : public ui::Widget {
public:
    static constexpr double kColumnWidth = 440.0;
    static constexpr double kRowHeight = 30.0;

    WelcomeView();

    void setOnOpen(std::function<void()> onOpen);
    void setOnOpenRecent(std::function<void(const std::filesystem::path&)> onOpenRecent) {
        onOpenRecent_ = std::move(onOpenRecent);
    }
    // Replaces the recent list; existence is checked here (missing = greyed).
    void setRecentFiles(const std::vector<std::filesystem::path>& paths);
    std::size_t recentCount() const { return recent_.size(); }
    bool recentExists(std::size_t index) const { return index < recent_.size() && recent_[index].exists; }

    // Row rect of recent entry `index` (local coordinates; tests).
    core::Rect recentRowRect(std::size_t index) const;
    ui::Button& openButton() { return *openButton_; }

    void layout() override;
    bool onMouse(const ui::PointerEvent& event) override;
    void paintSelf(ui::PaintContext& context) const override;

private:
    struct Entry {
        std::filesystem::path path;
        bool exists = false;
    };
    core::Rect column() const;
    double listTop() const;
    std::optional<std::size_t> rowAt(core::Point point) const;

    ui::Button* openButton_ = nullptr;
    std::vector<Entry> recent_;
    std::optional<std::size_t> hovered_;
    std::optional<std::size_t> pressed_;
    std::function<void(const std::filesystem::path&)> onOpenRecent_;
};

} // namespace rivet::app
