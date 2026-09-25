// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "ui/UiTypes.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace rivet::ui {

// One overlay rectangle painted OVER the page tiles (selection highlights,
// search matches, link hover). rect is in PAGE DISPLAY space (points,
// top-left origin, y-down) - the same space the render source rasters in.
struct OverlayRect {
    core::Rect rect;
    Color color;
};

// A link hit resolved by the bridge. Internal links navigate by page index
// (+ optional target point in page display space); external links carry the
// URL for the host's scheme-validated opener.
struct ViewerLinkHit {
    enum class Kind : std::uint8_t { Internal, External };
    Kind kind = Kind::Internal;
    std::size_t pageIndex = 0;      // Internal only
    bool hasPoint = false;          // Internal only
    core::Point point;              // page display points
    std::string url;                // External only
};

// Bridges the viewport to viewer text features (text hit testing, selection
// state, overlay content) WITHOUT a dependency on the editor layer. The app
// shell implements it over the active tab's text service + selection model
// and installs it on the viewport; a null bridge disables text interaction.
//
// Page coordinates: `pageIndex` is the layout index (0-based); page points
// are page DISPLAY space. Character indexes are page-local PdfTextPage
// indexes.
//
// All calls are on the main thread (viewport paint/input).
class IViewerTextBridge {
public:
    virtual ~IViewerTextBridge() = default;

    // Fire-and-forget extraction warm-up so hit testing on the page hits a
    // warm cache shortly after. Cheap to call repeatedly.
    virtual void warmPage(std::size_t pageIndex) = 0;

    // Nearest selectable character for a page display point, or nullopt when
    // the page's text is not loaded (or has no selectable characters).
    virtual std::optional<std::uint32_t> charIndexAtPoint(std::size_t pageIndex,
                                                          const core::Point& pagePoint) = 0;

    // Overlay rects for the page (selection highlights, search matches).
    virtual std::vector<OverlayRect> overlayRects(std::size_t pageIndex) = 0;

    // Selection lifecycle from the viewport:
    //   - began: mouse-down hit a character (shift -> extend from any
    //     existing selection's anchor semantics are the bridge's job).
    //   - moved: drag extended to another character.
    //   - ended: mouse released (drag complete).
    //   - a plain click that did not hit text clears the selection.
    virtual void selectionDragBegan(std::size_t pageIndex, std::uint32_t charIndex, bool shiftHeld) = 0;
    virtual void selectionDragMoved(std::size_t pageIndex, std::uint32_t charIndex) = 0;
    virtual void selectionDragEnded() = 0;
    virtual void selectionCleared() = 0;

    // Link interaction. The bridge resolves page links (they load
    // asynchronously; a page with links not yet loaded reports none). Hit
    // testing is in page display space.
    virtual std::optional<ViewerLinkHit> linkAtPoint(std::size_t pageIndex,
                                                     const core::Point& pagePoint) = 0;
    // Rects to stroke as a subtle hover indication (page display space).
    virtual std::vector<core::Rect> linkRects(std::size_t pageIndex) = 0;
    // Fired for a click on a link (mouse down + up over the same link).
    // The host navigates internally / opens external URLs only after this
    // EXPLICIT user interaction.
    virtual void linkActivated(const ViewerLinkHit& hit) = 0;
};

} // namespace rivet::ui
