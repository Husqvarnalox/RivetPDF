// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "ui/ViewportTool.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace rivet::app {

// Crop geometry of one page, in its UNCROPPED DISPLAY SPACE: the display
// space (points, top-left origin, y-down, rotation applied) of the view
// {view.rotation, mediaBox}. The whole media box is {0, 0, displaySize};
// the page's current crop box is a sub-rect of it. Because every view
// mapping is a rigid quarter-turn + flip, the page's CURRENT display space
// (what the viewport renders) is this space translated by the current crop
// rect's origin - so a crop rect can be edited over the rendered page and
// extend beyond it up to the media box.
struct CropFrame {
    pdf::PdfPageView view;   // the page's current view (rotation + crop box)
    pdf::PdfBox mediaBox;    // user space

    pdf::PdfPageView uncroppedView() const { return pdf::PdfPageView{view.rotation, mediaBox}; }
    // {0, 0, media display size}.
    core::Rect mediaRect() const;
    // The current crop box in uncropped display space.
    core::Rect currentCropRect() const;
    // Uncropped display rect -> user-space crop box (clamped to the media
    // box, so float slop never leaves it).
    pdf::PdfBox toUserBox(const core::Rect& uncroppedRect) const;
    // User-space box -> uncropped display rect (clamped to the media rect).
    core::Rect fromUserBox(const pdf::PdfBox& box) const;
};

// The crop tool: a ui::ViewportTool editing one page's crop box over the
// document view. Shows the dimmed media area outside the crop, the crop
// outline with 8 resize handles, and an Apply / Reset / Cancel bar.
//
// Interaction: drag a handle to resize (edges clamped to the media box and
// to kMinSizePoints), drag inside to move (clamped to the media box);
// Enter or "Apply" commits, "Reset" asks for the native crop, Esc or
// "Cancel" leaves. The tool never mutates the document: the owner receives
// the result through the callbacks and runs the command (and ends the tool).
//
// Hit testing is in viewport-local LOGICAL points with fixed handle sizes,
// so it is independent of the zoom and of the display's backing scale.
//
// Main thread only.
class CropTool final : public ui::ViewportTool {
public:
    static constexpr double kMinSizePoints = 18.0;   // page points
    static constexpr double kHandleSize = 8.0;       // logical points (drawn)
    static constexpr double kHandleHitRadius = 7.0;  // logical points
    static constexpr double kBarHeight = 30.0;
    static constexpr double kBarButtonWidth = 72.0;

    enum class Handle : std::uint8_t {
        None,
        Move,
        TopLeft,
        Top,
        TopRight,
        Right,
        BottomRight,
        Bottom,
        BottomLeft,
        Left,
    };

    struct Callbacks {
        std::function<void(const pdf::PdfBox& cropBox)> onApply;
        std::function<void()> onReset;
        std::function<void()> onCancel;
    };

    void setCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

    // Starts editing page `pageIndex` (layout order) whose current view and
    // media box are given. The edited rect starts at the current crop.
    void begin(std::size_t pageIndex, const CropFrame& frame);
    // Stops editing (no callbacks).
    void end();
    bool isActive() const { return active_; }
    std::size_t pageIndex() const { return pageIndex_; }
    const CropFrame& frame() const { return frame_; }

    // The edited rect in uncropped display space (page points).
    const core::Rect& cropRect() const { return cropRect_; }
    // Replaces the edited rect (normalized, clamped to the media rect and
    // the minimum size).
    void setCropRect(const core::Rect& rect);
    // The edited rect as a user-space crop box.
    pdf::PdfBox userCropBox() const { return frame_.toUserBox(cropRect_); }

    // Viewport mapping (nullopt when the page is not laid out).
    std::optional<core::Rect> cropRectInViewport(const ui::ViewportToolHost& host) const;
    std::optional<core::Point> toUncropped(const ui::ViewportToolHost& host,
                                           const core::Point& viewportPoint) const;
    Handle handleAt(const ui::ViewportToolHost& host, const core::Point& viewportPoint) const;

    struct BarRects {
        core::Rect bar;
        core::Rect apply;
        core::Rect reset;
        core::Rect cancel;
    };
    // The action bar, top-centered in the viewport.
    static BarRects barRects(const ui::ViewportToolHost& host);

    // Commit / reset / cancel (what the bar and Enter/Esc trigger).
    void apply();
    void reset();
    void cancel();

    // ui::ViewportTool
    bool onMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) override;
    bool onKey(ui::ViewportToolHost& host, const ui::KeyEvent& event) override;
    void paint(const ui::ViewportToolHost& host, ui::PaintContext& context) const override;

private:
    // Rect resulting from dragging `handle` by `delta` (uncropped points)
    // from `start`, clamped.
    core::Rect draggedRect(Handle handle, const core::Rect& start, const core::Point& delta) const;
    double minWidth() const;
    double minHeight() const;

    Callbacks callbacks_;
    bool active_ = false;
    std::size_t pageIndex_ = 0;
    CropFrame frame_;
    core::Rect base_;     // the crop rect the page is currently rendered with
    core::Rect cropRect_; // the edited rect

    Handle dragHandle_ = Handle::None;
    core::Rect dragStartRect_;
    core::Point dragStartPoint_;
};

} // namespace rivet::app
