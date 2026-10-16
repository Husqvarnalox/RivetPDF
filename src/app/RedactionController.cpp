// SPDX-License-Identifier: MPL-2.0
#include "app/RedactionController.hpp"

#include "editor/DocumentSession.hpp"
#include "editor/RedactionCommands.hpp"
#include "editor/SelectionText.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfText.hpp"
#include "ui/PdfViewport.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace rivet::app {

RedactionController::RedactionController(ShellContext& context, ContentController& content)
    : context_(context), content_(content), layer_(*this) {
    interaction_.setMarkHitTest([this](std::size_t page, core::Point display) -> std::optional<std::size_t> {
        const std::vector<core::Rect> rects = marks(page);
        for (std::size_t i = rects.size(); i > 0; --i) {
            if (rects[i - 1].contains(display)) return i - 1;
        }
        return std::nullopt;
    });
    context_.viewport.addLayer(&layer_);
}

RedactionController::~RedactionController() { context_.viewport.removeLayer(&layer_); }

void RedactionController::bindTab(DocumentTab* tab) {
    (void)tab;
    interaction_.cancel();
    interaction_.setSelection(std::nullopt);
}

void RedactionController::repaint() { context_.viewport.invalidate(); }

bool RedactionController::active() const {
    return content_.tool() == ContentTool::Redact && activeTab() != nullptr;
}

std::optional<core::Size> RedactionController::pageSize(std::size_t page) const {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return std::nullopt;
    const editor::PageSnapshotPtr snapshot = tab->session()->pageSnapshot();
    if (page >= snapshot->size()) return std::nullopt;
    return pdf::displaySize(snapshot->at(page).view);
}

std::vector<core::Rect> RedactionController::marks(std::size_t page) const {
    std::vector<core::Rect> rects;
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return rects;
    const editor::PageSnapshotPtr snapshot = tab->session()->pageSnapshot();
    if (page >= snapshot->size()) return rects;
    const editor::PageEntry& entry = snapshot->at(page);
    if (entry.redactionMarks == nullptr) return rects;
    rects.reserve(entry.redactionMarks->size());
    for (const pdf::PdfBox& box : *entry.redactionMarks) rects.push_back(pdf::userBoxToDisplayRect(entry.view, box));
    return rects;
}

bool RedactionController::run(core::Result<std::unique_ptr<editor::Command>> command) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return false;
    if (!command.has_value()) {
        context_.setStatus("Redact: " + command.error().message);
        return false;
    }
    if (*command == nullptr) return false; // nothing to do
    if (const core::Status status = tab->session()->execute(std::move(*command)); !status.has_value()) {
        context_.setStatus("Redact: " + status.error().message);
        return false;
    }
    repaint();
    return true;
}

void RedactionController::applyIntent(const RedactionInteraction::Intent& intent) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return;
    editor::DocumentSession& session = *tab->session();
    switch (intent.kind) {
    case RedactionInteraction::Intent::Kind::AddMark:
        if (intent.page < session.pageCount()) {
            run(editor::addRedactionMarks(session, {{session.pageId(intent.page), intent.rect}}));
        }
        break;
    case RedactionInteraction::Intent::Kind::RemoveMark:
        if (intent.page < session.pageCount()) {
            run(editor::removeRedactionMark(session, session.pageId(intent.page), intent.index));
        }
        break;
    case RedactionInteraction::Intent::Kind::SelectMark:
    case RedactionInteraction::Intent::Kind::ClearSelection:
    case RedactionInteraction::Intent::Kind::None:
        repaint();
        break;
    }
}

bool RedactionController::handleEscape() {
    if (!active()) return false;
    const RedactionInteraction::Intent intent = interaction_.key(ui::Key::Escape);
    if (!intent.consumed) return false;
    repaint();
    return true;
}

bool RedactionController::textSelectionNonEmpty() const {
    const DocumentTab* tab = activeTab();
    return tab != nullptr && !tab->selection().empty() && !tab->selection().selection().empty();
}

// --- Menus -----------------------------------------------------------------------

bool RedactionController::canPerform(RedactCommand command) const {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return false;
    switch (command) {
    case RedactCommand::ToolRedact: return true;
    case RedactCommand::MarkSelection: return textSelectionNonEmpty() && !tab->session()->isEditingLocked();
    case RedactCommand::ApplyRedactions:
    case RedactCommand::ClearMarks:
        return !tab->session()->isEditingLocked() && editor::redactionMarkCount(*tab->session()->pageSnapshot()) > 0;
    }
    return false;
}

std::optional<bool> RedactionController::isChecked(RedactCommand command) const {
    if (command != RedactCommand::ToolRedact) return std::nullopt;
    return content_.tool() == ContentTool::Redact;
}

void RedactionController::perform(RedactCommand command) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) {
        context_.setStatus("Open a document to redact");
        return;
    }
    switch (command) {
    case RedactCommand::ToolRedact:
        content_.toggleTool(ContentTool::Redact);
        interaction_.cancel();
        interaction_.setSelection(std::nullopt);
        if (content_.tool() == ContentTool::Redact) {
            context_.setStatus("Redact: drag over what to remove, then choose Apply Redactions");
        }
        repaint();
        break;
    case RedactCommand::MarkSelection: markSelection(); break;
    case RedactCommand::ApplyRedactions: applyMarks(); break;
    case RedactCommand::ClearMarks:
        interaction_.setSelection(std::nullopt);
        run(editor::clearRedactionMarks(*tab->session()));
        break;
    }
}

void RedactionController::markSelection() {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || !textSelectionNonEmpty()) return;
    editor::DocumentSession& session = *tab->session();
    const std::vector<editor::TextRange> ranges = editor::orderedSelectionRanges(
        tab->selection().selection(), [&session](core::PageId id) { return session.pageIndexFor(id); },
        [&session](std::size_t index) { return session.pageId(index); }, editor::DocumentSession::kInvalidPage);
    std::vector<editor::RedactionMarkDraft> drafts;
    bool missing = false;
    for (const editor::TextRange& range : ranges) {
        const std::shared_ptr<const pdf::PdfTextPage> text = session.textService().cachedTextPage(range.page);
        if (text == nullptr) {
            session.textService().ensureTextPage(range.page);
            missing = true;
            continue;
        }
        const std::uint32_t end = static_cast<std::uint32_t>(std::min<std::size_t>(range.end, text->charCount()));
        if (range.begin >= end) continue;
        for (const core::Rect& rect : text->rectsForRange(range.begin, end - range.begin)) {
            if (rect.size.width >= editor::kMinRedactionMarkExtent &&
                rect.size.height >= editor::kMinRedactionMarkExtent) {
                drafts.push_back(editor::RedactionMarkDraft{range.page, rect});
            }
        }
    }
    if (missing) {
        context_.setStatus("Redact: Text not ready yet");
        return;
    }
    if (drafts.empty()) {
        context_.setStatus("Redact: Nothing to mark");
        return;
    }
    if (run(editor::addRedactionMarks(session, std::move(drafts)))) {
        tab->selection().clear();
        context_.setStatus("Redact: marked; choose Apply Redactions to remove the text");
    }
}

void RedactionController::applyMarks() {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return;
    editor::DocumentSession& session = *tab->session();
    const std::size_t count = editor::redactionMarkCount(*session.pageSnapshot());
    if (count == 0) {
        context_.setStatus("Redact: Nothing is marked for redaction");
        return;
    }
    // Build first: a refusal (probe, loading, form field) is shown before
    // anything is asked.
    {
        auto probe = editor::applyRedactions(session, {});
        if (!probe.has_value()) {
            context_.setStatus("Redact: " + probe.error().message);
            if (context_.services.alerts != nullptr) {
                context_.services.alerts->showError("Cannot Apply Redactions", probe.error().message);
            }
            return;
        }
    }
    platform::IAlertService* alerts = context_.services.alerts;
    if (alerts == nullptr) {
        context_.setStatus("Redact: confirmation is not available");
        return;
    }
    const std::string areas = count == 1 ? std::string("1 marked area") : std::to_string(count) + " marked areas";
    if (!alerts->confirm("Apply Redactions?",
                         "Everything in " + areas +
                             " is removed from the page content: text, image pixels, drawings, links and form "
                             "fields. Until you save, Undo restores it; once saved, it cannot be recovered.",
                         "Apply Redactions")) {
        return;
    }
    editor::RedactionOptions options;
    if (!session.info().isEncrypted) {
        options.removeMetadata = alerts->confirm(
            "Also Remove Document Metadata?",
            "The saved file can also drop its title, author and other metadata. It is then rebuilt from its "
            "pages: bookmarks, form fields, page labels and tagging are not kept.",
            "Remove Metadata");
    }
    interaction_.setSelection(std::nullopt);
    if (run(editor::applyRedactions(session, options))) {
        context_.setStatus("Redact: applied; the content is removed from the file when you save");
    }
}

} // namespace rivet::app
