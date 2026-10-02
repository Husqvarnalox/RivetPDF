// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/AnnotationInteraction.hpp"
#include "editor/Annotations.hpp"
#include "ui/ViewportTool.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace rivet::app {

// What the layer needs from its owner (the AnnotationController): the
// interaction machine, the resolved annotations of a page, and a sink for
// intents. Kept narrow so the layer is testable with a stub.
class AnnotationLayerClient {
public:
    virtual ~AnnotationLayerClient() = default;

    virtual AnnotationInteraction& interaction() = 0;
    virtual const AnnotationInteraction& interaction() const = 0;
    // The selected annotation of the active tab as it resolves NOW (pruned
    // when it no longer exists), with its current geometry.
    virtual std::optional<AnnotationInteraction::Selected> currentSelection() const = 0;
    // Resolved annotations of the page at layout index `pageIndex` (null
    // when none / not loaded).
    virtual std::shared_ptr<const std::vector<editor::AnnotationView>>
    pageAnnotations(std::size_t pageIndex) const = 0;
    // The style a creation preview is drawn with (the current tool's).
    virtual editor::AnnotationStyle previewStyle() const = 0;
    virtual bool textSelectionNonEmpty() const = 0;
    // A primary press reached the layer: close transient UI (note editor).
    virtual void pointerPressed() = 0;
    virtual void applyIntent(const AnnotationInteraction::Intent& intent) = 0;
};

// The persistent annotation overlay of the viewport (ui::ViewportLayer). It
// converts viewport events into page-display-space inputs for the
// interaction machine, forwards the resulting intents to the client, and
// paints annotations, selection chrome and gesture previews. It never
// touches the document and executes no PDF actions.
class AnnotationLayer final : public ui::ViewportLayer {
public:
    // Seconds on any monotonic clock (double-click timing); injectable for tests.
    using Clock = std::function<double()>;

    static constexpr double kDoubleClickSeconds = 0.4;
    static constexpr double kDoubleClickDistance = 5.0; // logical points

    explicit AnnotationLayer(AnnotationLayerClient& client, Clock clock = {});

    bool onMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) override;
    void afterMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) override;
    bool onKey(ui::ViewportToolHost& host, const ui::KeyEvent& event) override;
    void paintPage(const ui::ViewportToolHost& host, std::size_t pageIndex, const core::Rect& pageRectInViewport,
                   ui::PaintContext& context) const override;
    void paintAbove(const ui::ViewportToolHost& host, ui::PaintContext& context) const override;

private:
    AnnotationInteraction::PointerInput inputFor(const ui::ViewportToolHost& host, std::size_t page,
                                                 core::Point viewportPoint, const ui::PointerEvent& event,
                                                 int clickCount) const;
    int clickCountFor(const ui::PointerEvent& event);

    AnnotationLayerClient& client_;
    Clock clock_;
    double lastDownTime_ = -1.0e9;
    core::Point lastDownPoint_;
    int lastClickCount_ = 0;
};

} // namespace rivet::app
