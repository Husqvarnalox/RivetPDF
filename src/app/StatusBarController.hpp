// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/ShellContext.hpp"

#include "core/geometry/Rect.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace rivet::ui {
class Container;
class TextField;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

class TextLabel;

// Zoom indicator text shown by the shell's toolbar: 1.0 -> "100%" (rounded
// to whole percent).
std::string zoomPercentText(double zoom);

// The status bar: a message label on the left and the page indicator
// "Page [field] / N" on the right. The field navigates the ACTIVE tab's
// viewport on Enter (strict digits only; anything else is ignored).
//
// Main thread only.
class StatusBarController {
public:
    static constexpr double kHeight = 24.0;
    // Size of the page indicator once it lives in the toolbar.
    static constexpr double kPageIndicatorWidth = 88.0;

    // Builds the bar into `parent` (appended as its next child).
    StatusBarController(ShellContext& context, ui::Widget& parent, std::string initialStatus);

    StatusBarController(const StatusBarController&) = delete;
    StatusBarController& operator=(const StatusBarController&) = delete;

    // Positions the bar (frame in the parent's space; kHiddenFrame hides it)
    // and its right-aligned page cluster.
    void layout(const core::Rect& frame);

    void setStatus(std::string text);
    const std::string& statusText() const;

    // Page-selection summary ("3 pages selected"; empty hides it), shown
    // left of the page indicator.
    void setSelectionSummary(std::string text);
    const std::string& selectionSummary() const;

    // Re-reads the active tab's page count and current page. The field text
    // is left alone while the field has focus (the user is editing it).
    void updatePageIndicator();

    // Enter in the page field: navigates to the typed page when it parses,
    // then returns focus to the viewport.
    void commitPageField();

    // Strict parse of a 1-based page number typed by the user: ASCII digits
    // only, no sign/whitespace, no overflow, 1 <= n <= pageCount. Returns
    // the 0-based page index, or nullopt when the text is not a valid page.
    static std::optional<std::size_t> parsePageNumber(std::string_view text, std::size_t pageCount);

    // Moves the page field and count into `host` (the toolbar's page
    // navigation group, ADR-0033 §5): field at x 0, "/ N" after it. The
    // status bar then shows only the status text and the selection summary.
    void attachPageIndicator(ui::Widget& host);

    ui::TextField& pageField() { return *pageField_; }
    const std::string& pageCountText() const;

private:
    ShellContext& context_;
    // Raw pointers into widgets owned by the parent's tree.
    ui::Container* bar_ = nullptr;
    TextLabel* statusLabel_ = nullptr;
    TextLabel* selectionLabel_ = nullptr;
    TextLabel* pageCaptionLabel_ = nullptr;
    ui::TextField* pageField_ = nullptr;
    TextLabel* pageCountLabel_ = nullptr;
    bool pageIndicatorAttached_ = false;
};

} // namespace rivet::app
