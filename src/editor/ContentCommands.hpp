// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "editor/Command.hpp"
#include "editor/ContentObjects.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PageCommands.hpp"

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfContent.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Content edits as undoable commands (ADR-0014/0015). Every factory builds
// the page's NEW immutable PdfPageContentEdits from the current one (plus
// a validation against the page's extracted content), and the command swaps
// the page entry's edits pointer, minting a fresh contentRevision AND
// rasterRevision for that page on every execute/undo/redo (tiles, text,
// search and links of that page are invalidated; nothing else is).
//
// Factories fail (Result error) when the page's content is not loaded yet,
// the object/block is unknown, its capability forbids the operation, the
// session is editing-locked, or the resulting edits do not validate.
// Geometry arguments are DISPLAY space of the page's current view; the
// factories map them to user space through the centralized view mapping.

namespace rivet::editor {

class ContentEditsCommand final : public PageCommand {
public:
    struct Swap {
        core::PageId page;
        pdf::PdfPageContentEditsPtr before;
        pdf::PdfPageContentEditsPtr after;
    };
    ContentEditsCommand(PageModel& model, std::string name, std::vector<Swap> swaps);
    std::string_view name() const override { return name_; }
    bool execute() override;
    bool undo() override;

private:
    bool apply(bool forward);
    std::string name_;
    std::vector<Swap> swaps_;
};

struct ContentEdit {
    std::unique_ptr<Command> command;
    std::vector<core::ObjectId> ids; // objects/blocks the command addresses (created: the new block id)
};

// Objects and/or blocks (a block id moves all its members, or the block's
// placement when it is a Rivet block).
core::Result<ContentEdit> moveContent(DocumentSession& session, core::PageId page, std::vector<core::ObjectId> ids,
                                      core::Point displayDelta);
core::Result<ContentEdit> deleteContent(DocumentSession& session, core::PageId page, std::vector<core::ObjectId> ids);
// Images and paths: geometric scaling into `displayBounds` (axis-aligned in
// display space; the object's frame is mapped so its display bounds become
// `displayBounds`). Text blocks refuse (use setTextBlockWrapWidth).
core::Result<ContentEdit> resizeContent(DocumentSession& session, core::PageId page, core::ObjectId id,
                                        core::Rect displayBounds);
// Image replacement (decoded by the platform; validated against the limits).
core::Result<ContentEdit> replaceImage(DocumentSession& session, core::PageId page, core::ObjectId id,
                                       std::shared_ptr<const pdf::PdfImageData> image);

// Text. Editing an existing block (ADR-0015 "replace in place"): new text,
// optional new size/color; keeps the block's font (FromObject) and layout.
struct TextBlockPatch {
    std::optional<std::string> text;       // UTF-8
    std::optional<double> fontSize;        // effective size
    std::optional<pdf::PdfColor> color;
    std::optional<double> wrapWidth;       // user-space points, 0 = none
    std::optional<pdf::PdfBundledFont> font; // Rivet blocks only (Bundled)
};
core::Result<ContentEdit> editTextBlock(DocumentSession& session, core::PageId page, core::ObjectId blockId,
                                        TextBlockPatch patch);

// Add Text: a new block at `displayOrigin` (the first baseline start), with
// a bundled font. `displayWrapWidth` 0 = no wrapping. The returned id is
// the block's ObjectId (= the edit tag), stable across undo/redo.
struct NewTextBlock {
    std::string text; // UTF-8, non-empty
    pdf::PdfBundledFont font = pdf::PdfBundledFont::SansRegular;
    double fontSize = 12.0;
    pdf::PdfColor color;
    core::Point displayOrigin;
    double displayWrapWidth = 0.0;
};
core::Result<ContentEdit> addTextBlock(DocumentSession& session, core::PageId page, NewTextBlock block);

// Z-order helpers (optional per spec): bring the object(s) to the front or
// send to the back by re-ordering through transforms is NOT possible in the
// edit model; these are provided only for Rivet-created blocks (front is
// the default) and return NotAvailable otherwise. Documented limitation.
core::Result<ContentEdit> bringToFront(DocumentSession& session, core::PageId page, core::ObjectId id);

// Characters the bundled face cannot write (empty = all covered). Used by
// the UI to refuse a commit with a clear message before building a command.
std::u32string uncoveredCodepoints(pdf::PdfBundledFont font, std::string_view utf8);

} // namespace rivet::editor
