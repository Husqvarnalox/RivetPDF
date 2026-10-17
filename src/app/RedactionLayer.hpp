// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/RedactionInteraction.hpp"
#include "ui/Theme.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/ViewportTool.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rivet::app {

// Redaction commands the shell exposes to platform menus (integers on the
// platform side: keep the values stable).
enum class RedactCommand : std::uint8_t {
    ToolRedact = 0,      // toggles ContentTool::Redact (check state)
    MarkSelection = 1,   // marks the text selection, one area per line rect
    ApplyRedactions = 2, // confirmation, then Apply Redactions (one undo step)
    ClearMarks = 3,      // removes every mark (one undo step)
};

// What the redaction layer needs from its owner (the RedactionController).
// Kept narrow so the layer is testable with a stub; every rect is in the
// page DISPLAY space of the page at layout index `page`.
class RedactionLayerClient {
public:
    virtual ~RedactionLayerClient() = default;

    // True while the Redact tool is active on a Ready PDF tab (otherwise the
    // layer ignores input; marks are painted regardless).
    virtual bool active() const = 0;
    virtual RedactionInteraction& interaction() = 0;
    virtual const RedactionInteraction& interaction() const = 0;
    // Display size of a page (nullopt for an unknown page).
    virtual std::optional<core::Size> pageSize(std::size_t page) const = 0;
    // The page's marks as display rects, in mark order.
    virtual std::vector<core::Rect> marks(std::size_t page) const = 0;
    virtual void applyIntent(const RedactionInteraction::Intent& intent) = 0;
};

// The redaction overlay of the viewport (ADR-0026): outlines every mark in
// red, the selected one thicker, and the rectangle being dragged; while the
// Redact tool is active it turns primary drags into marks. It never touches
// the document.
class RedactionLayer final : public ui::ViewportLayer {
public:
    explicit RedactionLayer(RedactionLayerClient& client) : client_(client) {}

    bool onMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) override;
    void afterMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) override;
    bool onKey(ui::ViewportToolHost& host, const ui::KeyEvent& event) override;
    void paintPage(const ui::ViewportToolHost& host, std::size_t pageIndex, const core::Rect& pageRectInViewport,
                   ui::PaintContext& context) const override;
    void paintAbove(const ui::ViewportToolHost& host, ui::PaintContext& context) const override;

    static ui::Color markColor() { return ui::colors().redactionMark; }

private:
    RedactionLayerClient& client_;
};

} // namespace rivet::app
