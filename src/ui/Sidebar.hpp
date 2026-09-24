#pragma once

#include "ui/Widget.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rivet::ui {

// Vertical text-row list used for document info / the page list in the shell.
// Rows are laid out from the top-left padding downward with a fixed height;
// the widget does not scroll (the shell sizes it to fit).
class Sidebar : public Widget {
public:
    static constexpr double kRowHeight = 24.0;
    static constexpr double kPadding = 10.0;
    static constexpr double kRowCornerRadius = 5.0;
    static constexpr double kRowTextInset = 8.0;

    Sidebar() = default;

    // Replaces the rows. Keeps the selection when still valid, drops it
    // otherwise.
    void setItems(std::vector<std::string> items);
    const std::vector<std::string>& items() const { return items_; }

    void setSelectedIndex(std::optional<std::size_t> index);
    std::optional<std::size_t> selectedIndex() const { return selectedIndex_; }

    // Fired after the selection changes (not when the same row is re-picked).
    void setOnSelectionChanged(std::function<void(std::size_t)> onSelectionChanged);

    // Down picks the row under the point; other events are not consumed.
    bool onMouse(const PointerEvent& event) override;

    // Row index under the point (local coordinates), or nullopt.
    std::optional<std::size_t> rowIndexAt(const core::Point& localPoint) const;

    void paintSelf(PaintContext& context) const override;

private:
    core::Rect rowRect(std::size_t index) const;

    std::vector<std::string> items_;
    std::optional<std::size_t> selectedIndex_;
    std::function<void(std::size_t)> onSelectionChanged_;
};

} // namespace rivet::ui
