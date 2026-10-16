// SPDX-License-Identifier: MPL-2.0
#pragma once

// True redaction (ADR-0026) as undoable commands: mark, review, apply.
//
//   - Marks are a per-page immutable list on PageEntry::redactionMarks (user
//     space of the entry's source page). RedactionMarksCommand swaps the
//     pointers of the pages involved; marks change nothing in the content.
//   - ApplyRedactionsCommand is ONE undo step that turns every page's marks
//     into content edits (pdf::PdfPageContentEdits::redactions), removes the
//     annotations (links, widgets, popups, markup, ...) whose rect intersects
//     a mark through the page's annotation state, clears the values of form
//     fields none of whose widgets survives, optionally flags the document
//     metadata for removal at save, and clears the marks. It is all or
//     nothing: a page that cannot be redacted (regeneration probe, too many
//     objects or annotations, content still loading, a field whose value
//     cannot be removed) refuses the whole apply with the reason - never a
//     partial redaction. Undo restores everything until the document is
//     saved.
//
// Every factory takes DISPLAY-space input of the page's current view and is
// main-thread only.

#include "editor/Command.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PageCommands.hpp"
#include "editor/PageModel.hpp"

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfContent.hpp"
#include "pdf/PdfForms.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::editor {

class RedactionMarksCommand final : public PageCommand {
public:
    struct Swap {
        core::PageId page;
        RedactionMarksPtr before;
        RedactionMarksPtr after;
    };

    RedactionMarksCommand(PageModel& model, std::string name, std::vector<Swap> swaps);

    std::string_view name() const override { return name_; }
    bool execute() override;
    bool undo() override;

private:
    bool apply(bool forward);

    std::string name_;
    std::vector<Swap> swaps_;
};

class ApplyRedactionsCommand final : public PageCommand {
public:
    struct PageState {
        core::PageId page;
        pdf::PdfPageContentEditsPtr edits;
        PageAnnotationStatePtr annotations;
        RedactionMarksPtr marks;
    };
    struct DocumentState {
        pdf::PdfFormValuesPtr formValues;
        bool removeMetadata = false;
    };

    // `before` and `after` are parallel (same pages, same order). Like every
    // content edit, each execute/undo/redo mints fresh content and raster
    // revisions for the pages (nothing derived from them is reused).
    ApplyRedactionsCommand(PageModel& model, std::vector<PageState> before, std::vector<PageState> after,
                           DocumentState documentBefore, DocumentState documentAfter);

    std::string_view name() const override { return "Apply Redactions"; }
    bool execute() override;
    bool undo() override;

private:
    bool apply(bool forward);

    std::vector<PageState> before_;
    std::vector<PageState> after_;
    DocumentState documentBefore_;
    DocumentState documentAfter_;
};

// A new mark: a display-space rectangle of `page`'s current view.
struct RedactionMarkDraft {
    core::PageId page;
    core::Rect displayRect;
};

// Smallest side (display points) a mark may have.
inline constexpr double kMinRedactionMarkExtent = 1.0;

// Adds marks (one undo step, any pages). InvalidArgument for a degenerate
// or non-finite rect or more than kMaxRedactionMarksPerPage marks on a page;
// NotFound for an unknown page; Unsupported while editing is locked.
core::Result<std::unique_ptr<Command>> addRedactionMarks(DocumentSession& session,
                                                         std::vector<RedactionMarkDraft> drafts);

// Removes mark `index` of `page` (display order = application order).
core::Result<std::unique_ptr<Command>> removeRedactionMark(DocumentSession& session, core::PageId page,
                                                           std::size_t index);

// Removes every mark of the document; nullptr when there is none.
core::Result<std::unique_ptr<Command>> clearRedactionMarks(DocumentSession& session);

// Number of marks over all pages of the snapshot.
std::size_t redactionMarkCount(const PageModelSnapshot& snapshot);

struct RedactionOptions {
    pdf::PdfColor fill;          // default black
    bool removeMetadata = false; // drop /Info and XMP at the next save
};

// Why the marks of `page` cannot be applied right now (content or
// annotations still loading, probe refusal, too many objects/annotations);
// nullopt when they can (or the page has no marks). Main thread.
std::optional<core::Error> redactionBlocker(DocumentSession& session, core::PageId page);

// Builds the Apply Redactions command for every marked page (see above).
// InvalidArgument when nothing is marked; NotAvailable while something is
// still loading; Unsupported when a page or a form field cannot be
// redacted (the message names the page).
core::Result<std::unique_ptr<Command>> applyRedactions(DocumentSession& session, const RedactionOptions& options);

} // namespace rivet::editor
