// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/PaintContext.hpp"
#include "ui/Widget.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rivet::ui {

// Horizontal strip of document tabs (ADR-0033 §5): title, dirty dot, close
// button on hover; the active tab is filled with the toolbar color so it
// visually joins the toolbar below.
// Tab widths come from title measurement clamped to [kMinTabWidth,
// kMaxTabWidth]; titles that do not fit are ellipsized. Tabs beyond the
// strip's width are not painted and do not hit-test (documented simple
// overflow policy; no drag-reordering).
//
// Geometry note: measurement requires a PaintContext, so tab rectangles are
// computed during paintSelf from that context and cached; hit-testing uses
// the cache. layout() does nothing (it has no context and must not
// re-measure), so a resize only refreshes the cache on the next paint.
// Priming with a paint call is expected before interaction in tests;
// production always paints before input.
class TabStrip final : public Widget {
public:
    static constexpr double kTabHeight = 32.0;      // the strip's height
    static constexpr double kTabTopInset = 5.0;     // tabs hang from the top inset
    static constexpr double kLeadingInset = 8.0;    // first tab's x
    static constexpr double kTabGap = 2.0;
    static constexpr double kTabLeftPadding = 10.0;
    static constexpr double kMinTabWidth = 96.0;
    static constexpr double kMaxTabWidth = 220.0;
    static constexpr double kCloseButtonSize = 16.0;

    struct Tab {
        std::string title;
        bool dirty = false; // unsaved changes: a dot is drawn before the close button
    };

    // Replaces the model. activeIndex beyond the list resets to nullopt.
    void setTabs(std::vector<Tab> tabs, std::optional<std::size_t> activeIndex);
    std::size_t tabCount() const { return tabs_.size(); }
    std::optional<std::size_t> activeIndex() const { return activeIndex_; }
    void setActiveIndex(std::optional<std::size_t> index); // invalid -> nullopt

    // Click on a tab body (not its close button). The host updates the
    // active index via setActiveIndex.
    void setOnTabActivated(std::function<void(std::size_t)> onTabActivated);
    // Click (down inside, up inside) on a tab's close button.
    void setOnTabCloseRequested(std::function<void(std::size_t)> onTabCloseRequested);

    core::Size preferredSize(const PaintContext& context) const override;

    bool onMouse(const PointerEvent& event) override;
    void paintSelf(PaintContext& context) const override;

    // Visible tab index under the local point, or nullopt (tests/diagnostics).
    // Returns nullopt before the first paint (no layout cache yet) and for
    // indices at or beyond the visible count.
    std::optional<std::size_t> tabIndexAt(const core::Point& localPoint) const;
    // Close-button rect of visible tab `index` in local coordinates (tests).
    // Precondition: index is below the visible count. Returns an empty rect
    // when the strip has not been laid out yet.
    core::Rect closeButtonRect(std::size_t index) const;

private:
    // Measured title + paddings + close button, clamped to
    // [kMinTabWidth, kMaxTabWidth].
    double tabWidthForTitle(const std::string& title, const PaintContext& context) const;
    // Recomputes tabRects_/visibleCount_ from the paint context. Called from
    // paintSelf only; input handling relies on the cached result.
    void layoutTabs(const PaintContext& context) const;
    // Title text drawn in a tab of the given width (ellipsized when needed).
    std::string displayTitle(const std::string& title, double tabWidth,
                             const PaintContext& context) const;

    std::vector<Tab> tabs_;
    std::optional<std::size_t> activeIndex_;
    // Layout cache: one rect per tab (culled tabs keep their laid-out x for
    // diagnostics); visibleCount_ is the number of tabs fully inside the
    // strip width. Refreshed on every paint.
    mutable std::vector<core::Rect> tabRects_;
    mutable std::size_t visibleCount_ = 0;
    std::optional<std::size_t> pressedClose_; // armed close button (tab index)
    std::optional<std::size_t> hoveredTab_;   // close button shows on hover
    bool hoveredClose_ = false;
    std::function<void(std::size_t)> onTabActivated_;
    std::function<void(std::size_t)> onTabCloseRequested_;
};

} // namespace rivet::ui
