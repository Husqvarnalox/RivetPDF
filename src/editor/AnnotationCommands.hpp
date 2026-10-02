// SPDX-License-Identifier: MPL-2.0
#pragma once

// Undoable annotation edits (Phase 4, ADR-0011). Every edit is one
// AnnotationStateCommand: it swaps the (state pointer, rasterRevision) of the
// affected pages, validating that the model still holds exactly the "before"
// pair (a stale command fails without touching the model). Redo re-applies
// the very same after-states, so ids, states and revisions are identical to
// the first execution. Dirty state comes only from CommandStack::stateId.
//
// The factories compute the after-states from the session's current page
// snapshot and the AnnotationService (which must have loaded the originals of
// the pages involved; an annotation that is unknown, deleted or whose page
// has not finished loading yields NotFound). All inputs are DISPLAY space of
// the page's current view; stored data is converted to user space here.
// Capabilities are enforced (AnnotationView::caps): an edit the annotation's
// kind does not allow is InvalidArgument.

#include "editor/AnnotationService.hpp"
#include "editor/Annotations.hpp"
#include "editor/Command.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PageCommands.hpp"

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAnnotation.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rivet::editor {

class AnnotationStateCommand final : public PageCommand {
public:
    struct Swap {
        core::PageId page;
        PageAnnotationStatePtr beforeState;
        std::uint64_t beforeRaster = 0;
        PageAnnotationStatePtr afterState;
        std::uint64_t afterRaster = 0;
    };

    AnnotationStateCommand(PageModel& model, std::string name, std::vector<Swap> swaps);

    std::string_view name() const override { return name_; }
    bool execute() override;
    bool undo() override;

private:
    bool apply(bool forward);

    std::string name_;
    std::vector<Swap> swaps_;
};

// A new annotation, in display space of `page`'s current view. Which
// geometry is read depends on `kind` (see pdf::PdfAnnotationData).
struct AnnotationDraft {
    core::PageId page;
    pdf::PdfAnnotationKind kind = pdf::PdfAnnotationKind::Other;
    std::vector<DisplayQuad> quads;                    // markup
    core::Rect rect;                                   // Note, Square, Circle, Stamp
    std::vector<std::vector<core::Point>> strokes;     // Ink
    core::Point lineStart;                             // Line, Arrow
    core::Point lineEnd;
    AnnotationStyle style;
    std::string contents; // UTF-8
    std::string author;   // UTF-8
    pdf::PdfStampName stampName = pdf::PdfStampName::Approved;
};

// Fields left empty stay unchanged. `interiorColor`: outer optional = change
// it, inner = the new value (nullopt = no fill).
struct StylePatch {
    std::optional<pdf::PdfColor> color;
    std::optional<std::optional<pdf::PdfColor>> interiorColor;
    std::optional<float> opacity;
    std::optional<float> borderWidth;
};

// A ready-to-execute command and the annotations it creates or edits (in
// request order).
struct AnnotationEdit {
    std::unique_ptr<Command> command;
    std::vector<core::AnnotationId> ids;
};

// One undo step for all drafts (any pages). Ids are minted here, so redo
// re-creates the same ids. The raster revision does not change.
core::Result<AnnotationEdit> createAnnotations(DocumentSession& session, std::vector<AnnotationDraft> drafts);

// Original: its /Annots index (and its popup's) is suppressed from the
// raster. Overlay item: dropped (an index it holds in the file stays
// suppressed).
core::Result<AnnotationEdit> deleteAnnotations(DocumentSession& session, std::vector<core::AnnotationId> ids);

// Display-space translation (needs kCapMove).
core::Result<AnnotationEdit> moveAnnotation(DocumentSession& session, core::AnnotationId id, core::Point delta);

// Fits the annotation's bounds into `displayBounds` (needs kCapResize).
// Square/Circle/Stamp: the rect. Ink/Line/Arrow: the points are scaled from
// the old tight point box to the new one (the margin between points and
// bounds - half the stroke width, arrow head - is kept, so repeated resizes
// do not drift).
core::Result<AnnotationEdit> resizeAnnotation(DocumentSession& session, core::AnnotationId id,
                                              core::Rect displayBounds);

// Line/Arrow only (needs kCapResize).
core::Result<AnnotationEdit> setLineEndpoints(DocumentSession& session, core::AnnotationId id,
                                              core::Point start, core::Point end);

// Needs kCapRestyle on every id. Fields that do not apply to a kind (e.g.
// borderWidth on a highlight) are ignored for it; InvalidArgument when the
// patch changes nothing at all.
core::Result<AnnotationEdit> restyleAnnotations(DocumentSession& session, std::vector<core::AnnotationId> ids,
                                                StylePatch patch);

// Needs kCapEditContents. InvalidArgument for invalid UTF-8 or more than
// pdf::kMaxContentsBytes.
core::Result<AnnotationEdit> editContents(DocumentSession& session, core::AnnotationId id, std::string utf8);

} // namespace rivet::editor
