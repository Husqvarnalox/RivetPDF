// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/AnnotationInteraction.hpp"
#include "editor/Annotations.hpp"
#include "ui/PaintContext.hpp"

namespace rivet::app {

// Stateless painting of annotation content and chrome (ADR-0013). Page
// display space maps to the viewport as `pageRect.origin + p * zoom`;
// widths scale with the zoom; the appearance's own opacity multiplies every
// color's alpha. Nothing here reads the document: callers pass resolved
// views.
class AnnotationPainter {
public:
    // The accent used for selection chrome and edit previews.
    static ui::Color accentColor();
    static ui::Color toColor(const pdf::PdfColor& color, double alpha = 1.0);
    // Page display point -> viewport point.
    static core::Point toViewport(const core::Rect& pageRect, double zoom, core::Point displayPoint);
    static core::Rect toViewport(const core::Rect& pageRect, double zoom, const core::Rect& displayRect);

    // Paints an overlay-drawn annotation (nothing for raster-drawn ones or
    // views without an appearance): paths in order, then texts.
    static void paintAnnotation(ui::PaintContext& context, const editor::AnnotationView& view,
                                const core::Rect& pageRect, double zoom);

    // Dashed bounds plus the handles the annotation's capabilities allow:
    // eight resize handles, or the two endpoint handles of a line.
    static void paintSelection(ui::PaintContext& context, const AnnotationInteraction::Selected& selected,
                               const core::Rect& pageRect, double zoom);

    // The running gesture: a creation preview drawn with `style`, or the
    // moved/resized outline of the edited annotation.
    static void paintPreview(ui::PaintContext& context, const AnnotationInteraction::Preview& preview,
                             const editor::AnnotationStyle& style, const core::Rect& pageRect, double zoom);
};

} // namespace rivet::app
