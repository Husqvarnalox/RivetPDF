// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/ShellContext.hpp"
#include "ui/ViewerTextBridge.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rivet::app {

// Viewer text features over the ACTIVE Ready tab: implements the viewport's
// IViewerTextBridge (text hit testing, selection lifecycle, overlay rects for
// selection + search highlights, link hit testing/activation) and owns the
// copy command and link navigation. Everything forwards to a no-op when no
// Ready tab is active.
//
// Page coordinates are layout indexes + page display points (the bridge
// contract); identities stored in models are PageIds.
//
// Main thread only. Asynchronous results (copy) arrive through the session's
// services and die with the session.
class TextInteractionController final : public ui::IViewerTextBridge {
public:
    explicit TextInteractionController(ShellContext& context) : context_(context) {}

    // Binds a freshly activated Ready tab: selection changes repaint the view.
    void bindTab(DocumentTab& tab);

    // Copies the active tab's selection (asynchronous, complete or failed).
    void copySelection();

    // Navigation targets.
    void navigateInternalDestination(std::size_t pageIndex, const core::Point& targetPoint,
                                     bool hasPoint);
    void openExternalUrl(const std::string& url);

    // IViewerTextBridge
    void warmPage(std::size_t pageIndex) override;
    std::optional<std::uint32_t> charIndexAtPoint(std::size_t pageIndex,
                                                  const core::Point& pagePoint) override;
    std::vector<ui::OverlayRect> overlayRects(std::size_t pageIndex) override;
    void selectionDragBegan(std::size_t pageIndex, std::uint32_t charIndex, bool shiftHeld) override;
    void selectionDragMoved(std::size_t pageIndex, std::uint32_t charIndex) override;
    void selectionDragEnded() override;
    void selectionCleared() override;
    std::optional<ui::ViewerLinkHit> linkAtPoint(std::size_t pageIndex,
                                                 const core::Point& pagePoint) override;
    std::vector<core::Rect> linkRects(std::size_t pageIndex) override;
    void linkActivated(const ui::ViewerLinkHit& hit) override;

private:
    ShellContext& context_;
};

} // namespace rivet::app
