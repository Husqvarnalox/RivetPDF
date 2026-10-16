// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "ui/ViewportTool.hpp"

#include <cstddef>
#include <cstdint>

namespace rivet::app {

// Menu commands of the form controller (integers on the platform side: keep
// the values stable).
enum class FormCommand : std::uint8_t {
    ResetForm = 0,       // Edit > Reset Form: restore every stored value (one undo step)
    HighlightFields = 1, // View > Highlight Form Fields (check state)
};

// What the form layer needs from its owner (the FormController). Kept narrow
// so the layer is testable with a stub; every point is in the page DISPLAY
// space of the page at layout index `page`.
class FormLayerClient {
public:
    virtual ~FormLayerClient() = default;

    // False: the layer ignores input (another tool than Select is active, no
    // Ready PDF tab); it still paints.
    virtual bool interactive() const = 0;
    // The choice popup, when open, in viewport-local coordinates.
    virtual bool popupOpen() const = 0;
    // A primary press inside the open popup's frame (true = consumed;
    // a press outside closes it and is consumed too).
    virtual bool popupPress(const ui::ViewportToolHost& host, core::Point viewportPoint) = 0;
    // A primary press on a page: true when it hit a fillable widget (focus /
    // edit / toggle / popup); false lets the press through (an open editor is
    // committed and the focus cleared).
    virtual bool press(std::size_t page, core::Point display) = 0;
    // A primary press outside every page.
    virtual void pressOutside() = 0;
    // Pointer movement (hover feedback); `onPage` false = off every page.
    virtual void hover(bool onPage, std::size_t page, core::Point display) = 0;
    virtual bool key(const ui::KeyEvent& event) = 0;
    // Paint hooks (viewport-local coordinates).
    virtual void paintPage(const ui::ViewportToolHost& host, std::size_t page, const core::Rect& pageRect,
                           ui::PaintContext& context) const = 0;
    virtual void paintAbove(const ui::ViewportToolHost& host, ui::PaintContext& context) const = 0;
    // Called once per paint pass (before paintAbove): re-places the inline
    // editor for the current scroll and zoom (the editor widget is painted
    // after the viewport, so the new frame takes effect in the same pass).
    virtual void placeEditor(const ui::ViewportToolHost& host) = 0;
};

// The AcroForm layer of the viewport (ADR-0024): a ui::ViewportLayer placed
// BELOW the annotation layer (it sees events first and paints first). It
// converts viewport events into page display space for its client and
// forwards painting. It never touches the document.
class FormLayer final : public ui::ViewportLayer {
public:
    explicit FormLayer(FormLayerClient& client) : client_(client) {}

    bool onMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) override;
    void afterMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) override;
    bool onKey(ui::ViewportToolHost& host, const ui::KeyEvent& event) override;
    void paintPage(const ui::ViewportToolHost& host, std::size_t pageIndex, const core::Rect& pageRectInViewport,
                   ui::PaintContext& context) const override;
    void paintAbove(const ui::ViewportToolHost& host, ui::PaintContext& context) const override;

private:
    FormLayerClient& client_;
};

} // namespace rivet::app
