// SPDX-License-Identifier: MPL-2.0
#include "editor/RedactionCommands.hpp"

#include "editor/AnnotationGeometry.hpp"
#include "editor/AnnotationService.hpp"
#include "editor/ContentObjects.hpp"
#include "editor/ContentService.hpp"
#include "editor/FormService.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace rivet::editor {
namespace {

core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "editor");
}

core::Error notFound(std::string message) {
    return core::makeError(core::ErrorCode::NotFound, std::move(message), "editor");
}

core::Error unsupported(std::string message) {
    return core::makeError(core::ErrorCode::Unsupported, std::move(message), "editor");
}

core::Error notAvailable(std::string message) {
    return core::makeError(core::ErrorCode::NotAvailable, std::move(message), "editor");
}

core::Status lockedStatus(const DocumentSession& session) {
    if (!session.isEditingLocked()) return core::ok();
    return std::unexpected(unsupported(session.editingLockReason().empty()
                                           ? std::string("the document cannot be edited right now")
                                           : session.editingLockReason()));
}

std::string pageLabel(std::size_t index) { return "page " + std::to_string(index + 1); }

bool intersects(const pdf::PdfBox& a, const pdf::PdfBox& b) {
    return a.left < b.right && b.left < a.right && a.bottom < b.top && b.bottom < a.top;
}

bool intersectsAny(const RedactionMarks& marks, const pdf::PdfBox& box) {
    // Annotation rects may be stored denormalized or empty (a zero-width
    // link): normalize and treat degenerate rects as their outline.
    // A rect with a non-finite component cannot be placed: remove it.
    if (!std::isfinite(box.left) || !std::isfinite(box.bottom) || !std::isfinite(box.right) ||
        !std::isfinite(box.top)) {
        return true;
    }
    pdf::PdfBox normalized = box;
    if (normalized.left > normalized.right) std::swap(normalized.left, normalized.right);
    if (normalized.bottom > normalized.top) std::swap(normalized.bottom, normalized.top);
    for (const pdf::PdfBox& mark : marks) {
        if (intersects(mark, normalized)) return true;
        const bool degenerate = !(normalized.right > normalized.left) || !(normalized.top > normalized.bottom);
        if (degenerate && normalized.right >= mark.left && normalized.left <= mark.right &&
            normalized.top >= mark.bottom && normalized.bottom <= mark.top) {
            return true;
        }
    }
    return false;
}

bool finiteBox(const pdf::PdfBox& box) {
    return std::isfinite(box.left) && std::isfinite(box.bottom) && std::isfinite(box.right) &&
           std::isfinite(box.top);
}

bool contains(const std::vector<std::uint32_t>& sorted, std::uint32_t value) {
    return std::binary_search(sorted.begin(), sorted.end(), value);
}

void insertSorted(std::vector<std::uint32_t>& sorted, std::uint32_t value) {
    const auto it = std::lower_bound(sorted.begin(), sorted.end(), value);
    if (it == sorted.end() || *it != value) sorted.insert(it, value);
}

// Why `view` (the resolved content of the page at model index `index`)
// cannot take redactions; nullopt when it can.
std::optional<core::Error> contentBlocker(const PageContentView& view, std::size_t index) {
    if (!view.loaded) return notAvailable("the content of " + pageLabel(index) + " is still loading");
    if (view.truncated) {
        return unsupported(pageLabel(index) + " cannot be redacted: it has too many objects");
    }
    if (view.hasEmbeddedThumbnail) {
        return unsupported(pageLabel(index) + " cannot be redacted: page has an embedded thumbnail Rivet cannot remove");
    }
    if (!view.regenerationSafe) {
        return unsupported(pageLabel(index) + " cannot be redacted: " +
                           (view.regenerationIssue.empty() ? std::string("its content cannot be rewritten safely")
                                                           : view.regenerationIssue));
    }
    return std::nullopt;
}

// XFA datasets keep field values that Rivet cannot clear.
constexpr const char* kXfaReason =
    "the document contains XFA form data, which Rivet cannot redact";

std::optional<core::Error> annotationBlocker(const pdf::PdfPageAnnotationsPtr& originals, std::size_t index) {
    if (originals == nullptr) return notAvailable("the annotations of " + pageLabel(index) + " are still loading");
    if (originals->annotsCount > originals->items.size()) {
        return unsupported(pageLabel(index) + " cannot be redacted: it has too many annotations");
    }
    return std::nullopt;
}

// The value that removes whatever a field holds; nullopt for types whose
// stored appearance cannot be cleared through the form filler.
std::optional<pdf::PdfFormValue> emptyValueFor(const pdf::PdfFormField& field) {
    switch (field.type) {
    case pdf::PdfFormFieldType::Text: return pdf::PdfFormValue::ofText({});
    case pdf::PdfFormFieldType::CheckBox:
    case pdf::PdfFormFieldType::RadioButton: return pdf::PdfFormValue::ofToggle({});
    case pdf::PdfFormFieldType::ComboBox:
    case pdf::PdfFormFieldType::ListBox: return pdf::PdfFormValue::ofChoice({});
    case pdf::PdfFormFieldType::PushButton:
    case pdf::PdfFormFieldType::Signature:
    case pdf::PdfFormFieldType::Unknown: break;
    }
    return std::nullopt;
}

} // namespace

// --- RedactionMarksCommand ----------------------------------------------------

RedactionMarksCommand::RedactionMarksCommand(PageModel& model, std::string name, std::vector<Swap> swaps)
    : PageCommand(model), name_(std::move(name)), swaps_(std::move(swaps)) {}

bool RedactionMarksCommand::apply(bool forward) {
    const PageSnapshotPtr snapshot = model_.snapshot();
    std::vector<PageModel::MarksUpdate> updates;
    updates.reserve(swaps_.size());
    for (const Swap& swap : swaps_) {
        const PageEntry* entry = snapshot->find(swap.page);
        if (entry == nullptr) return fail(notFound("page is not in the document"));
        const RedactionMarksPtr& expected = forward ? swap.before : swap.after;
        if (entry->redactionMarks != expected) {
            return fail(invalid("the page's redaction marks changed since the command was created"));
        }
        updates.push_back(PageModel::MarksUpdate{swap.page, forward ? swap.after : swap.before});
    }
    return record(model_.updateRedactionMarks(updates));
}

bool RedactionMarksCommand::execute() { return apply(true); }
bool RedactionMarksCommand::undo() { return apply(false); }

// --- ApplyRedactionsCommand ---------------------------------------------------

ApplyRedactionsCommand::ApplyRedactionsCommand(PageModel& model, std::vector<PageState> before,
                                               std::vector<PageState> after, DocumentState documentBefore,
                                               DocumentState documentAfter)
    : PageCommand(model),
      before_(std::move(before)),
      after_(std::move(after)),
      documentBefore_(std::move(documentBefore)),
      documentAfter_(std::move(documentAfter)) {}

bool ApplyRedactionsCommand::apply(bool forward) {
    const std::vector<PageState>& expected = forward ? before_ : after_;
    const std::vector<PageState>& target = forward ? after_ : before_;
    const DocumentState& expectedDocument = forward ? documentBefore_ : documentAfter_;
    const DocumentState& targetDocument = forward ? documentAfter_ : documentBefore_;
    if (expected.size() != target.size() || expected.empty()) return fail(invalid("nothing to redact"));
    const PageSnapshotPtr snapshot = model_.snapshot();
    if (snapshot->formValues() != expectedDocument.formValues ||
        snapshot->removeMetadata() != expectedDocument.removeMetadata) {
        return fail(invalid("the document changed since the redaction was prepared"));
    }
    for (const PageState& state : expected) {
        const PageEntry* entry = snapshot->find(state.page);
        if (entry == nullptr) return fail(notFound("page is not in the document"));
        if (entry->contentEdits != state.edits || entry->annotations != state.annotations ||
            entry->redactionMarks != state.marks) {
            return fail(invalid("the page changed since the redaction was prepared"));
        }
    }
    std::vector<PageModel::RedactionUpdate> updates;
    updates.reserve(target.size());
    for (const PageState& state : target) {
        updates.push_back(PageModel::RedactionUpdate{state.page, state.edits, model_.mintContentRevision(),
                                                     model_.mintRasterRevision(), state.annotations, state.marks});
    }
    return record(model_.updateRedaction(updates, targetDocument.formValues, targetDocument.removeMetadata));
}

bool ApplyRedactionsCommand::execute() { return apply(true); }
bool ApplyRedactionsCommand::undo() { return apply(false); }

// --- Mark factories -------------------------------------------------------------

core::Result<std::unique_ptr<Command>> addRedactionMarks(DocumentSession& session,
                                                         std::vector<RedactionMarkDraft> drafts) {
    if (core::Status locked = lockedStatus(session); !locked) return std::unexpected(locked.error());
    if (drafts.empty()) return std::unexpected(invalid("nothing to mark"));
    const PageSnapshotPtr snapshot = session.pageSnapshot();
    std::vector<core::PageId> order;
    std::unordered_map<core::PageId, RedactionMarks> work;
    for (const RedactionMarkDraft& draft : drafts) {
        const PageEntry* entry = snapshot->find(draft.page);
        if (entry == nullptr) return std::unexpected(notFound("page is not in the document"));
        const core::Rect& rect = draft.displayRect;
        if (!rect.isFinite() || !(rect.size.width >= kMinRedactionMarkExtent) ||
            !(rect.size.height >= kMinRedactionMarkExtent)) {
            return std::unexpected(invalid("the area to redact is too small"));
        }
        pdf::PdfBox box = geometry::rectToUserBox(entry->view, rect);
        if (box.left > box.right) std::swap(box.left, box.right);
        if (box.bottom > box.top) std::swap(box.bottom, box.top);
        if (!finiteBox(box) || !(box.right > box.left) || !(box.top > box.bottom)) {
            return std::unexpected(invalid("the area to redact is not valid"));
        }
        auto [it, inserted] = work.try_emplace(draft.page);
        if (inserted) {
            order.push_back(draft.page);
            if (entry->redactionMarks != nullptr) it->second = *entry->redactionMarks;
        }
        if (it->second.size() >= kMaxRedactionMarksPerPage) {
            return std::unexpected(invalid("too many areas are marked for redaction on the page"));
        }
        it->second.push_back(box);
    }
    std::vector<RedactionMarksCommand::Swap> swaps;
    swaps.reserve(order.size());
    for (const core::PageId page : order) {
        swaps.push_back(RedactionMarksCommand::Swap{page, snapshot->find(page)->redactionMarks,
                                                    std::make_shared<const RedactionMarks>(std::move(work[page]))});
    }
    return std::make_unique<RedactionMarksCommand>(session.pageModel(), "Mark for Redaction", std::move(swaps));
}

core::Result<std::unique_ptr<Command>> removeRedactionMark(DocumentSession& session, core::PageId page,
                                                           std::size_t index) {
    if (core::Status locked = lockedStatus(session); !locked) return std::unexpected(locked.error());
    const PageSnapshotPtr snapshot = session.pageSnapshot();
    const PageEntry* entry = snapshot->find(page);
    if (entry == nullptr) return std::unexpected(notFound("page is not in the document"));
    if (entry->redactionMarks == nullptr || index >= entry->redactionMarks->size()) {
        return std::unexpected(notFound("no such redaction mark"));
    }
    RedactionMarks after = *entry->redactionMarks;
    after.erase(after.begin() + static_cast<std::ptrdiff_t>(index));
    std::vector<RedactionMarksCommand::Swap> swaps;
    swaps.push_back(RedactionMarksCommand::Swap{
        page, entry->redactionMarks, after.empty() ? nullptr : std::make_shared<const RedactionMarks>(std::move(after))});
    return std::make_unique<RedactionMarksCommand>(session.pageModel(), "Remove Redaction Mark", std::move(swaps));
}

core::Result<std::unique_ptr<Command>> clearRedactionMarks(DocumentSession& session) {
    if (core::Status locked = lockedStatus(session); !locked) return std::unexpected(locked.error());
    const PageSnapshotPtr snapshot = session.pageSnapshot();
    std::vector<RedactionMarksCommand::Swap> swaps;
    for (const PageEntry& entry : snapshot->entries()) {
        if (entry.redactionMarks != nullptr) {
            swaps.push_back(RedactionMarksCommand::Swap{entry.id, entry.redactionMarks, nullptr});
        }
    }
    if (swaps.empty()) return std::unique_ptr<Command>{};
    return std::make_unique<RedactionMarksCommand>(session.pageModel(), "Clear Redaction Marks", std::move(swaps));
}

std::size_t redactionMarkCount(const PageModelSnapshot& snapshot) {
    std::size_t count = 0;
    for (const PageEntry& entry : snapshot.entries()) {
        if (entry.redactionMarks != nullptr) count += entry.redactionMarks->size();
    }
    return count;
}

// --- Apply ------------------------------------------------------------------------

std::optional<core::Error> redactionBlocker(DocumentSession& session, core::PageId page) {
    if (const pdf::PdfFormInfoPtr info = session.forms().info(); info != nullptr && info->hasXfa) {
        return unsupported(kXfaReason);
    }
    const PageSnapshotPtr snapshot = session.pageSnapshot();
    const std::size_t index = snapshot->indexOf(page);
    if (index == PageModelSnapshot::kInvalidIndex) return notFound("page is not in the document");
    if (const PageContentViewPtr view = session.contentService().content(page); view == nullptr) {
        return notAvailable("the content of " + pageLabel(index) + " is still loading");
    } else if (auto blocked = contentBlocker(*view, index)) {
        return blocked;
    }
    return annotationBlocker(session.annotations().originals(page), index);
}

core::Result<std::unique_ptr<Command>> applyRedactions(DocumentSession& session, const RedactionOptions& options) {
    if (core::Status locked = lockedStatus(session); !locked) return std::unexpected(locked.error());
    const auto unit = [](float v) { return std::isfinite(v) && v >= 0.0F && v <= 1.0F; };
    if (!unit(options.fill.r) || !unit(options.fill.g) || !unit(options.fill.b)) {
        return std::unexpected(invalid("the redaction color is out of range"));
    }
    const PageSnapshotPtr snapshot = session.pageSnapshot();
    PageModel& model = session.pageModel();

    // Pages first: every one must be redactable (never a partial redaction).
    std::vector<ApplyRedactionsCommand::PageState> before;
    std::vector<ApplyRedactionsCommand::PageState> after;
    // Per marked entry: /Annots indices this apply removes.
    std::unordered_map<core::PageId, std::vector<std::uint32_t>> newlyRemoved;
    std::unordered_map<core::PageId, PageAnnotationStatePtr> annotationsAfter;
    for (std::size_t index = 0; index < snapshot->size(); ++index) {
        const PageEntry& entry = snapshot->at(index);
        if (entry.redactionMarks == nullptr || entry.redactionMarks->empty()) continue;
        const RedactionMarks& marks = *entry.redactionMarks;

        const PageContentViewPtr view = session.contentService().content(entry.id);
        if (view == nullptr) return std::unexpected(notAvailable("the content of " + pageLabel(index) + " is still loading"));
        if (auto blocked = contentBlocker(*view, index)) return std::unexpected(std::move(*blocked));
        const pdf::PdfPageAnnotationsPtr originals = session.annotations().originals(entry.id);
        if (auto blocked = annotationBlocker(originals, index)) return std::unexpected(std::move(*blocked));

        // Content edits: the current ones plus one redaction per mark.
        auto edits = entry.contentEdits != nullptr ? std::make_shared<pdf::PdfPageContentEdits>(*entry.contentEdits)
                                                   : std::make_shared<pdf::PdfPageContentEdits>();
        for (const pdf::PdfBox& mark : marks) edits->redactions.push_back(pdf::PdfRedaction{mark, options.fill});
        if (core::Status valid = pdf::validate(*edits, view->sourceObjectCount); !valid) {
            return std::unexpected(invalid(pageLabel(index) + " cannot be redacted: " + valid.error().message));
        }

        // Annotations: originals (and their popups) and overlay items whose
        // rect intersects a mark are removed.
        PageAnnotationState state;
        if (entry.annotations != nullptr) state = *entry.annotations;
        std::vector<std::uint32_t>& removedHere = newlyRemoved[entry.id];
        for (const pdf::PdfPageAnnotation& item : originals->items) {
            if (!intersectsAny(marks, item.data.rect)) continue;
            for (const std::optional<std::uint32_t>& removed : {std::optional<std::uint32_t>(item.index), item.popupIndex}) {
                if (!removed.has_value() || contains(state.suppressed, *removed)) continue;
                insertSorted(state.suppressed, *removed);
                removedHere.push_back(*removed);
            }
        }
        std::erase_if(state.overlay,
                      [&](const OverlayAnnotation& item) { return intersectsAny(marks, item.data.rect); });
        PageAnnotationStatePtr annotations = entry.annotations;
        const bool annotationsChanged =
            entry.annotations == nullptr ? (!state.suppressed.empty() || !state.overlay.empty())
                                         : (state.suppressed != entry.annotations->suppressed ||
                                            state.overlay.size() != entry.annotations->overlay.size());
        if (annotationsChanged) {
            annotations = state.suppressed.empty() && state.overlay.empty()
                              ? nullptr
                              : std::make_shared<const PageAnnotationState>(std::move(state));
        }
        annotationsAfter[entry.id] = annotations;

        before.push_back(ApplyRedactionsCommand::PageState{entry.id, entry.contentEdits, entry.annotations,
                                                           entry.redactionMarks});
        after.push_back(ApplyRedactionsCommand::PageState{entry.id, std::move(edits), std::move(annotations), nullptr});
    }
    if (before.empty()) return std::unexpected(invalid("nothing is marked for redaction"));

    // Form fields of the base whose last presented widget this apply removes
    // lose their value (their field dictionary, reachable from the
    // document's /AcroForm, would otherwise keep it in the file).
    pdf::PdfFormValuesPtr formsAfter = snapshot->formValues();
    FormService& forms = session.forms();
    forms.ensureLoaded();
    if (forms.state() != FormService::State::Ready) {
        return std::unexpected(notAvailable("the document's form is still loading"));
    }
    const pdf::PdfFormInfoPtr info = forms.info();
    if (info != nullptr && info->hasXfa) return std::unexpected(unsupported(kXfaReason));
    if (info != nullptr && !info->empty()) {
        // Entries presenting each base page.
        std::map<std::size_t, std::vector<const PageEntry*>> presenting;
        for (const PageEntry& entry : snapshot->entries()) {
            if (entry.source != nullptr && entry.source == snapshot->base()) {
                presenting[entry.sourcePageIndex].push_back(&entry);
            }
        }
        const auto suppressedAfter = [&](const PageEntry& entry) -> const std::vector<std::uint32_t>* {
            const auto it = annotationsAfter.find(entry.id);
            const PageAnnotationStatePtr& state = it != annotationsAfter.end() ? it->second : entry.annotations;
            return state != nullptr ? &state->suppressed : nullptr;
        };
        std::optional<pdf::PdfFormValues> values;
        for (const pdf::PdfFormField& field : info->fields) {
            bool removedNow = false;
            bool visible = false;
            for (const pdf::PdfFormWidget& widget : field.widgets) {
                const auto pages = presenting.find(widget.pageIndex);
                if (pages == presenting.end()) continue;
                for (const PageEntry* entry : pages->second) {
                    const std::vector<std::uint32_t>* suppressed = suppressedAfter(*entry);
                    if (suppressed == nullptr || !contains(*suppressed, widget.annotIndex)) {
                        visible = true;
                        continue;
                    }
                    const auto removed = newlyRemoved.find(entry->id);
                    if (removed != newlyRemoved.end() &&
                        std::find(removed->second.begin(), removed->second.end(), widget.annotIndex) !=
                            removed->second.end()) {
                        removedNow = true;
                    }
                }
            }
            if (!removedNow || visible) continue;
            std::optional<pdf::PdfFormValue> empty = emptyValueFor(field);
            if (!empty.has_value()) {
                return std::unexpected(unsupported(
                    "a redacted area contains a button or signature field, whose appearance cannot be removed"));
            }
            if (field.hasParentField) {
                // ADR-0026: never under-redact. The default (/DV) or rich-text
                // (/RV) value of a field with a separate parent dictionary
                // cannot be read or removed through PDFium's public API.
                return std::unexpected(unsupported(
                    "a redacted area contains a form field whose default value cannot be removed"));
            }
            empty->clearDefaults = field.defaultResidue;
            if (!values.has_value()) values = formsAfter != nullptr ? *formsAfter : pdf::PdfFormValues{};
            if (pdf::originalValue(field) == *empty && !field.defaultResidue) {
                values->erase(field.name); // the file holds nothing to remove
                continue;
            }
            if (!info->fillable() || !pdf::validateFormValue(field, *empty).has_value()) {
                return std::unexpected(unsupported(
                    "a redacted area contains a read-only form field whose value cannot be removed"));
            }
            (*values)[field.name] = *empty;
        }
        if (values.has_value()) {
            formsAfter = values->empty() ? nullptr : std::make_shared<const pdf::PdfFormValues>(std::move(*values));
        }
    }

    ApplyRedactionsCommand::DocumentState documentBefore{snapshot->formValues(), snapshot->removeMetadata()};
    ApplyRedactionsCommand::DocumentState documentAfter{std::move(formsAfter),
                                                        snapshot->removeMetadata() || options.removeMetadata};
    return std::make_unique<ApplyRedactionsCommand>(model, std::move(before), std::move(after),
                                                    std::move(documentBefore), std::move(documentAfter));
}

} // namespace rivet::editor
