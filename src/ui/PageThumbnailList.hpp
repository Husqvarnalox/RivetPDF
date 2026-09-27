// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/StrongId.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "render/PhysicalRenderScaleKey.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"
#include "ui/ScrollBar.hpp"
#include "ui/Widget.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rivet::render {
class PageLayout;
class IRenderSource;
} // namespace rivet::render

namespace rivet::ui {

// Vertical list of page thumbnails with lazy, virtualized rendering.
//
// Rendering strategy: thumbnails go through the SAME IRenderSource the
// viewport uses - one whole-page raster per page at a sidebar-fixed physical
// scale, so a normal page renders as a single low-resolution raster. Requests
// are made at Prefetch priority (Impending for the selected page's tile) so
// they can never delay visible viewport tiles (the DocumentRenderer drains
// higher priorities first). Cache identity is regular TileCache identity at
// the thumbnail scale key, so thumbnail rasters share the bounded tile cache;
// no second image cache exists.
//
// Virtualization: only the visible row range (plus a small margin) is
// painted; off-screen thumbnails are never requested. Row geometry comes from
// a precomputed prefix-sum table (O(log n) lookups), so documents with
// thousands of pages cost nothing until scrolled into view.
//
// Scrolling: internal ScrollBar + wheel events; no zoom (scale is 1 in this
// widget).
//
// Two independent row states:
//   - the CURRENT row (setSelectedIndex; the page the viewport tracks):
//     subtle gray fill, as before page editing existed;
//   - the PAGE SELECTION (setPageSelection; owned by the app's
//     editor::PageSelection): accent fill for selected rows, accent outline
//     for the active row.
// The widget never changes the page selection itself: it reports user
// intents (row clicks with their gesture, keyboard navigation, delete,
// select-all, drag-and-drop moves) and the owner pushes the resulting
// selection back synchronously.
//
// Drag and drop: pressing a SELECTED row (after the click gesture was
// applied) and moving more than kDragThreshold starts a drag of the whole
// selection. While dragging, the drop GAP (0 = before the first row,
// rowCount = after the last) follows the pointer and is shown as an
// insertion line; moving within kAutoscrollBand of the top/bottom edge
// scrolls the list (per pointer event - there is no timer). Release fires
// onMoveRequested(gap); Esc (key) cancels. A release outside the widget is
// never delivered, so the next button-less move or press cancels the drag.
//
// Lifetime: the layout/source must be kept alive by the shell or released
// via clearDocument(). Completion callbacks are guarded by a shared alive
// flag like PdfViewport's.
class PageThumbnailList : public Widget {
public:
    static constexpr double kRowPadding = 8.0;       // padding around each row
    static constexpr double kThumbnailWidth = 160.0; // points
    static constexpr double kLabelHeight = 18.0;     // page label strip under the image
    static constexpr std::size_t kVisibleMarginRows = 2; // rows requested beyond the viewport
    static constexpr double kDragThreshold = 4.0;        // points before a press becomes a drag
    static constexpr double kAutoscrollBand = 28.0;      // edge band that autoscrolls a drag
    static constexpr double kAutoscrollMaxStep = 24.0;   // points per pointer event at the edge

    // How a row click should change the page selection.
    enum class ClickGesture : std::uint8_t {
        Replace, // plain click: select exactly this row
        Toggle,  // Cmd/Ctrl-click: toggle this row
        Extend,  // Shift-click: range from the anchor to this row
    };

    PageThumbnailList();
    ~PageThumbnailList() override;

    // Binds a document. layout/source may be null (empty state).
    void setDocument(core::DocumentId documentId, const render::PageLayout* layout,
                     render::IRenderSource* source,
                     std::function<std::uint64_t()> revisionProvider);
    void clearDocument();

    // The selected (current) page row; highlighted and rendered at higher
    // priority. Out-of-range resets to nullopt.
    void setSelectedIndex(std::optional<std::size_t> index);
    std::optional<std::size_t> selectedIndex() const { return selectedIndex_; }

    // Fired after the user clicks a row (not for programmatic selection).
    void setOnSelectionChanged(std::function<void(std::size_t)> onSelectionChanged);

    // Page selection visuals: `selected` is parallel to the rows (shorter =
    // the rest unselected); `active` is the keyboard-focus row.
    void setPageSelection(std::vector<bool> selected, std::optional<std::size_t> active);
    bool isRowSelected(std::size_t index) const {
        return index < selectedRows_.size() && selectedRows_[index];
    }
    std::optional<std::size_t> activeRow() const { return activeRow_; }

    // User intents (see class comment).
    void setOnRowClicked(std::function<void(std::size_t row, ClickGesture gesture)> onRowClicked);
    // Arrow keys: move the active row by `delta` rows; `extend` = Shift.
    void setOnNavigate(std::function<void(int delta, bool extend)> onNavigate);
    void setOnDeleteRequested(std::function<void()> onDeleteRequested);
    void setOnSelectAllRequested(std::function<void()> onSelectAllRequested);
    void setOnMoveRequested(std::function<void(std::size_t gap)> onMoveRequested);
    // A press inside the list wants keyboard focus (the host routes focus).
    void setOnFocusRequested(std::function<void()> onFocusRequested);

    // The bound layout changed in place (page model edit): rebuilds the row
    // geometry, keeps the scroll offset (clamped) and cancels a drag. The
    // current row and page selection are left to the owner to re-push.
    void reloadPages();

    // Drag state (tests / painting).
    bool isDragging() const { return dragging_; }
    std::optional<std::size_t> dropGap() const {
        return dragging_ ? std::optional<std::size_t>{dropGap_} : std::nullopt;
    }
    void cancelDrag();
    double scrollOffset() const { return scrollOffset_; }

    // Drop gap for a point in LIST-LOCAL coordinates: the gap before the row
    // under the point when above its middle, after it otherwise.
    std::size_t gapAt(const core::Point& localPoint) const;
    // Row frame in list-local coordinates (scroll applied). Asserts range.
    core::Rect rowFrame(std::size_t index) const;

    // Page labels from the PDF page-label tree ("i", "A-1", ...); rows with
    // an empty label fall back to "Page N". Call after setDocument.
    void setPageLabels(std::vector<std::string> labels);

    // Scrolls the given page row fully into view. Called by the shell when
    // the tracked current page changes so the list follows the document.
    void revealPage(std::size_t index);

    bool onMouse(const PointerEvent& event) override;
    // Arrows/Shift+arrows navigate, Delete/Backspace delete, Cmd/Ctrl+A
    // selects all, Esc cancels a drag. Only while focused (host routing).
    bool onKey(const KeyEvent& event) override;
    bool wantsFocus() const override { return true; }
    void layout() override;
    void paintSelf(PaintContext& context) const override;

private:
    // Row geometry. Row height = thumbnail height (aspect of the page) +
    // label strip + padding; tops come from the prefix-sum table.
    double contentHeight() const;
    core::Rect thumbnailRect(std::size_t index) const;
    core::Rect rowRect(std::size_t index) const;
    std::size_t rowCount() const;
    std::pair<std::size_t, std::size_t> visibleRowRange() const;
    std::optional<std::size_t> rowIndexAt(const core::Point& localPoint) const;

    void rebuildRowGeometry();
    void setScrollOffset(double offset);
    void updateDrag(const core::Point& localPoint);
    void syncScrollbar();
    void positionScrollbar();
    void requestThumbnail(std::size_t index, double backingScale,
                          render::RenderPriority priority) const;
    render::PhysicalRenderScaleKey thumbnailScaleKey(std::size_t index, double backingScale) const;

    core::DocumentId documentId_;
    const render::PageLayout* layout_ = nullptr;
    render::IRenderSource* source_ = nullptr;
    std::function<std::uint64_t()> revisionProvider_;

    std::optional<std::size_t> selectedIndex_;
    std::function<void(std::size_t)> onSelectionChanged_;

    std::vector<bool> selectedRows_;
    std::optional<std::size_t> activeRow_;
    std::function<void(std::size_t, ClickGesture)> onRowClicked_;
    std::function<void(int, bool)> onNavigate_;
    std::function<void()> onDeleteRequested_;
    std::function<void()> onSelectAllRequested_;
    std::function<void(std::size_t)> onMoveRequested_;
    std::function<void()> onFocusRequested_;

    // Press / drag state.
    std::optional<std::size_t> pressedRow_;
    core::Point pressPoint_;
    bool pressCanDrag_ = false;
    bool deferredReplace_ = false; // plain press on a selected row: Replace on release
    bool dragging_ = false;
    std::size_t dropGap_ = 0;
    std::vector<std::string> pageLabels_;

    // Prefix sums of row heights (prefixHeights_[i] = top of row i;
    // prefixHeights_.back() + last height = total content height).
    std::vector<double> prefixHeights_;
    double scrollOffset_ = 0.0;

    ScrollBar* scrollBar_ = nullptr;

    // Alive flag shared with thumbnail completion callbacks (same pattern as
    // PdfViewport).
    std::shared_ptr<std::atomic<bool>> aliveFlag_;
};

} // namespace rivet::ui
