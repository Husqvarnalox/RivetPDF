// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/ContentInteraction.hpp"
#include "ui/ViewportTool.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <optional>

namespace rivet::app {

// What the layer needs from its owner (the ContentController). Kept narrow
// so the layer is testable with a stub.
class ContentLayerClient {
public:
    virtual ~ContentLayerClient() = default;

    virtual ContentInteraction& interaction() = 0;
    virtual const ContentInteraction& interaction() const = 0;
    // True while a content tool is the active tool (otherwise the layer
    // ignores all input and paints nothing but a running editor's outline).
    virtual bool active() const = 0;
    // The selected object of the active tab as it resolves NOW (pruned when
    // it no longer exists), with its current geometry.
    virtual std::optional<ContentInteraction::Selected> currentSelection() const = 0;
    // A primary press reached the layer: commit the inline editor.
    virtual void pointerPressed() = 0;
    virtual void applyIntent(const ContentInteraction::Intent& intent, bool shift) = 0;
    // The block outline to show while the inline editor is open: page layout
    // index and display-space quad.
    struct EditingOutline {
        std::size_t page = 0;
        std::array<core::Point, 4> quad{};
    };
    virtual std::optional<EditingOutline> editingOutline() const = 0;
    // Called at the start of every layer paint: the owner re-places its
    // inline editor for the current scroll and zoom (the editor widget is
    // painted after the viewport, so the new frame takes effect in the same
    // pass).
    virtual void placeEditor(const ui::ViewportToolHost& host) = 0;
};

// The content overlay of the viewport (ui::ViewportLayer), the twin of
// AnnotationLayer. It converts viewport events into page-display-space
// inputs for the interaction machine, forwards the intents to the client
// and paints hover highlight, selection chrome (outline + handles) and
// gesture previews. The page content itself is rasterized by the backend;
// nothing here touches the document.
class ContentLayer final : public ui::ViewportLayer {
public:
    // Seconds on any monotonic clock (double-click timing); injectable for tests.
    using Clock = std::function<double()>;

    static constexpr double kDoubleClickSeconds = 0.4;
    static constexpr double kDoubleClickDistance = 5.0; // logical points

    explicit ContentLayer(ContentLayerClient& client, Clock clock = {});

    bool onMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) override;
    void afterMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) override;
    bool onKey(ui::ViewportToolHost& host, const ui::KeyEvent& event) override;
    void paintPage(const ui::ViewportToolHost& host, std::size_t pageIndex, const core::Rect& pageRectInViewport,
                   ui::PaintContext& context) const override;
    void paintAbove(const ui::ViewportToolHost& host, ui::PaintContext& context) const override;

private:
    ContentInteraction::PointerInput inputFor(const ui::ViewportToolHost& host, std::size_t page,
                                              core::Point viewportPoint, const ui::PointerEvent& event,
                                              int clickCount) const;
    int clickCountFor(const ui::PointerEvent& event);

    ContentLayerClient& client_;
    Clock clock_;
    double lastDownTime_ = -1.0e9;
    core::Point lastDownPoint_;
    int lastClickCount_ = 0;
};

} // namespace rivet::app
