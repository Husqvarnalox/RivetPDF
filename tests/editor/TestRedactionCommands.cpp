// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "fakes/ContentTestSupport.hpp"

#include "editor/RedactionCommands.hpp"
#include "pdf/PdfForms.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Redaction commands over the fake backend (ADR-0026): marks (add, remove,
// clear, undo/redo, limits) and Apply Redactions (content edits, annotation
// removal, form value clearing, metadata flag, all-or-nothing refusals).

using namespace rivet;
using namespace rivet::editor;
using namespace rivet::test;
using core::ErrorCode;
using core::PageId;
using core::Rect;

namespace {

// Display rect of the user-space box (left, bottom)-(right, top) on the
// 612 x 792 fake pages (native view).
Rect userRect(double left, double bottom, double right, double top) {
    return Rect{left, 792.0 - top, right - left, top - bottom};
}

pdf::PdfPageAnnotation opaqueAt(std::uint32_t index, double left, double bottom, double right, double top) {
    pdf::PdfPageAnnotation item;
    item.index = index;
    item.kind = pdf::PdfAnnotationKind::Other;
    item.data.kind = pdf::PdfAnnotationKind::Other;
    item.data.rect = pdf::PdfBox{left, bottom, right, top};
    return item;
}

pdf::PdfFormInfoPtr formWith(pdf::PdfFormFieldType type, std::string value, bool readOnly = false,
                             bool residue = false, bool parent = false) {
    auto info = std::make_shared<pdf::PdfFormInfo>();
    pdf::PdfFormField field;
    field.name = "secret";
    field.type = type;
    field.value = std::move(value);
    field.readOnly = readOnly;
    field.defaultResidue = residue;
    field.hasParentField = parent;
    pdf::PdfFormWidget widget;
    widget.pageIndex = 0;
    widget.annotIndex = 1;
    widget.rect = pdf::PdfBox{100, 690, 130, 710};
    field.widgets.push_back(widget);
    info->fields.push_back(std::move(field));
    return info;
}

// Page 0: sample content; annotations 0 = link (70,695)-(90,705), 1 = form
// widget (100,690)-(130,710), 2 = highlight far away (300,300)-(320,310).
struct RedactionFixture : ContentFixture {
    explicit RedactionFixture(pdf::PdfFormInfoPtr form = nullptr) {
        if (form != nullptr) document->setFormInfo(std::move(form));
        setContent(0, samplePage());
        document->setAnnotations(0, {opaqueAt(0, 70, 695, 90, 705), opaqueAt(1, 100, 690, 130, 710),
                                     makeOriginal(2, highlightAt(300, 300, 320, 310))});
    }

    // Content, annotations and form of page `index` loaded.
    bool ready(std::size_t index = 0) {
        if (!load(index)) return false;
        const PageId page = id(index);
        if (!settle(dispatcher, [&] { return session->annotations().originals(page) != nullptr; })) return false;
        session->forms().ensureLoaded();
        return settle(dispatcher, [&] { return session->forms().state() == FormService::State::Ready; });
    }

    bool runCommand(core::Result<std::unique_ptr<Command>> command) {
        if (!command.has_value() || *command == nullptr) return false;
        return session->execute(std::move(*command)).has_value();
    }

    bool mark(std::size_t index, Rect rect) { return runCommand(addRedactionMarks(*session, {{id(index), rect}})); }
};

bool hasCode(const core::Result<std::unique_ptr<Command>>& result, ErrorCode code) {
    return !result.has_value() && result.error().code == code;
}

} // namespace

// --- Marks --------------------------------------------------------------------------

RIVET_TEST(Redaction_marks_are_user_space_and_undoable) {
    RedactionFixture f;
    CHECK(f.runCommand(addRedactionMarks(*f.session, {{f.id(0), userRect(60, 680, 140, 720)},
                                                      {f.id(0), userRect(10, 10, 20, 20)},
                                                      {f.id(1), userRect(30, 30, 50, 50)}})));
    CHECK(f.entry(0).redactionMarks != nullptr);
    if (f.entry(0).redactionMarks == nullptr) return;
    CHECK_EQ(f.entry(0).redactionMarks->size(), std::size_t{2});
    const pdf::PdfBox& first = f.entry(0).redactionMarks->front();
    CHECK_NEAR(first.left, 60.0, 1e-9);
    CHECK_NEAR(first.bottom, 680.0, 1e-9);
    CHECK_NEAR(first.right, 140.0, 1e-9);
    CHECK_NEAR(first.top, 720.0, 1e-9);
    CHECK(f.entry(1).redactionMarks != nullptr);
    CHECK_EQ(redactionMarkCount(*f.session->pageSnapshot()), std::size_t{3});
    // Marks never touch the content.
    CHECK(f.entry(0).contentEdits == nullptr);

    CHECK(f.session->undo());
    CHECK(f.entry(0).redactionMarks == nullptr);
    CHECK(f.entry(1).redactionMarks == nullptr);
    CHECK(f.session->redo());
    CHECK_EQ(redactionMarkCount(*f.session->pageSnapshot()), std::size_t{3});

    CHECK(f.runCommand(removeRedactionMark(*f.session, f.id(0), 0)));
    CHECK_EQ(f.entry(0).redactionMarks->size(), std::size_t{1});
    CHECK_NEAR(f.entry(0).redactionMarks->front().left, 10.0, 1e-9);

    CHECK(f.runCommand(clearRedactionMarks(*f.session)));
    CHECK_EQ(redactionMarkCount(*f.session->pageSnapshot()), std::size_t{0});
    CHECK(f.session->undo());
    CHECK_EQ(redactionMarkCount(*f.session->pageSnapshot()), std::size_t{2});
    CHECK(f.session->undo());
    CHECK_EQ(redactionMarkCount(*f.session->pageSnapshot()), std::size_t{3});
}

RIVET_TEST(Redaction_marks_reject_invalid_input) {
    RedactionFixture f;
    CHECK(hasCode(addRedactionMarks(*f.session, {}), ErrorCode::InvalidArgument));
    CHECK(hasCode(addRedactionMarks(*f.session, {{f.id(0), Rect{10, 10, 0.2, 30}}}), ErrorCode::InvalidArgument));
    CHECK(hasCode(addRedactionMarks(*f.session, {{f.id(0), Rect{10, 10, std::numeric_limits<double>::infinity(), 30}}}),
                  ErrorCode::InvalidArgument));
    CHECK(hasCode(addRedactionMarks(*f.session, {{PageId{999999}, Rect{10, 10, 20, 30}}}), ErrorCode::NotFound));
    CHECK(hasCode(removeRedactionMark(*f.session, f.id(0), 0), ErrorCode::NotFound));
    // Clearing nothing is no command at all.
    auto none = clearRedactionMarks(*f.session);
    CHECK(none.has_value() && *none == nullptr);

    std::vector<RedactionMarkDraft> many(kMaxRedactionMarksPerPage, RedactionMarkDraft{f.id(0), Rect{10, 10, 5, 5}});
    CHECK(f.runCommand(addRedactionMarks(*f.session, many)));
    CHECK(hasCode(addRedactionMarks(*f.session, {{f.id(0), Rect{10, 10, 5, 5}}}), ErrorCode::InvalidArgument));
}

// --- Apply ----------------------------------------------------------------------------

RIVET_TEST(Redaction_apply_turns_marks_into_edits_and_removes_annotations_and_values) {
    RedactionFixture f(formWith(pdf::PdfFormFieldType::Text, "classified"));
    CHECK(f.ready());
    CHECK(f.mark(0, userRect(60, 680, 140, 720)));
    const std::uint64_t contentBefore = f.entry(0).contentRevision;

    RedactionOptions options;
    options.removeMetadata = true;
    auto command = applyRedactions(*f.session, options);
    CHECK(command.has_value());
    if (!command.has_value()) return;
    CHECK_EQ(std::string((*command)->name()), std::string("Apply Redactions"));
    CHECK(f.session->execute(std::move(*command)).has_value());

    const PageEntry& page = f.entry(0);
    CHECK(page.redactionMarks == nullptr);
    CHECK(page.contentRevision != contentBefore);
    CHECK(page.contentEdits != nullptr);
    if (page.contentEdits != nullptr) {
        CHECK_EQ(page.contentEdits->redactions.size(), std::size_t{1});
        CHECK_NEAR(page.contentEdits->redactions[0].area.left, 60.0, 1e-9);
        CHECK(page.contentEdits->redactions[0].fill == pdf::PdfColor{});
    }
    CHECK(page.annotations != nullptr);
    if (page.annotations != nullptr) {
        CHECK(page.annotations->suppressed == std::vector<std::uint32_t>({0, 1}));
    }
    // The field's only widget is gone: its value is removed from the save.
    const auto& values = f.session->pageSnapshot()->formValues();
    CHECK(values != nullptr);
    if (values != nullptr) {
        CHECK(values->contains("secret"));
        if (values->contains("secret")) CHECK(values->at("secret") == pdf::PdfFormValue::ofText(""));
    }
    CHECK(f.session->pageSnapshot()->removeMetadata());
    auto request = f.session->pageSnapshot()->toAssemblyRequest(PageModelSnapshot::AssemblyMode::Save);
    CHECK(request.has_value());
    if (request.has_value()) {
        CHECK(request->removeMetadata);
        CHECK(request->pages[0].contentEdits == page.contentEdits);
        CHECK(request->pages[0].annotationEdits != nullptr);
    }

    // One undo step restores everything.
    CHECK(f.session->undo());
    CHECK(f.entry(0).redactionMarks != nullptr);
    CHECK(f.entry(0).contentEdits == nullptr);
    CHECK(f.entry(0).annotations == nullptr);
    CHECK(f.session->pageSnapshot()->formValues() == nullptr);
    CHECK(!f.session->pageSnapshot()->removeMetadata());
    CHECK(f.session->redo());
    CHECK(f.entry(0).redactionMarks == nullptr);
    CHECK(f.entry(0).contentEdits != nullptr);
    CHECK(f.session->pageSnapshot()->removeMetadata());
}

RIVET_TEST(Redaction_apply_keeps_values_of_fields_with_surviving_widgets_and_untouched_annotations) {
    RedactionFixture f(formWith(pdf::PdfFormFieldType::Text, "classified"));
    CHECK(f.ready());
    // Only the link is covered.
    CHECK(f.mark(0, userRect(65, 690, 95, 712)));
    auto command = applyRedactions(*f.session, {});
    CHECK(command.has_value());
    if (!command.has_value()) return;
    CHECK(f.session->execute(std::move(*command)).has_value());
    CHECK(f.entry(0).annotations != nullptr);
    if (f.entry(0).annotations != nullptr) {
        CHECK(f.entry(0).annotations->suppressed == std::vector<std::uint32_t>({0}));
    }
    CHECK(f.session->pageSnapshot()->formValues() == nullptr);
    CHECK(!f.session->pageSnapshot()->removeMetadata());
}

RIVET_TEST(Redaction_apply_refuses_pages_the_probe_rejects) {
    RedactionFixture f;
    pdf::PdfPageContent content = samplePage();
    content.regenerationSafe = false;
    content.regenerationIssue = "page contains shading objects";
    f.setContent(1, content);
    CHECK(f.ready(0));
    CHECK(f.ready(1));
    CHECK(f.mark(0, userRect(60, 680, 140, 720)));
    CHECK(f.mark(1, userRect(60, 680, 140, 720)));
    const auto blocker = redactionBlocker(*f.session, f.id(1));
    CHECK(blocker.has_value());
    CHECK(!redactionBlocker(*f.session, f.id(0)).has_value());

    auto command = applyRedactions(*f.session, {});
    CHECK(hasCode(command, ErrorCode::Unsupported));
    if (!command.has_value()) {
        CHECK(command.error().message.find("page 2") != std::string::npos);
        CHECK(command.error().message.find("shading") != std::string::npos);
    }
    // Nothing was redacted anywhere (never partial).
    CHECK(f.entry(0).contentEdits == nullptr);
    CHECK(f.entry(0).redactionMarks != nullptr);
}

RIVET_TEST(Redaction_apply_refuses_while_loading_and_without_marks) {
    RedactionFixture f;
    CHECK(hasCode(applyRedactions(*f.session, {}), ErrorCode::InvalidArgument));
    CHECK(f.mark(0, userRect(60, 680, 140, 720)));
    // Content not requested yet.
    CHECK(hasCode(applyRedactions(*f.session, {}), ErrorCode::NotAvailable));
    CHECK(f.ready());
    RedactionOptions bad;
    bad.fill = pdf::PdfColor{2.0F, 0.0F, 0.0F};
    CHECK(hasCode(applyRedactions(*f.session, bad), ErrorCode::InvalidArgument));
    CHECK(applyRedactions(*f.session, {}).has_value());
}

RIVET_TEST(Redaction_apply_refuses_fields_whose_value_cannot_be_removed) {
    {
        RedactionFixture f(formWith(pdf::PdfFormFieldType::Text, "classified", /*readOnly=*/true));
        CHECK(f.ready());
        CHECK(f.mark(0, userRect(60, 680, 140, 720)));
        CHECK(hasCode(applyRedactions(*f.session, {}), ErrorCode::Unsupported));
    }
    {
        RedactionFixture f(formWith(pdf::PdfFormFieldType::Signature, ""));
        CHECK(f.ready());
        CHECK(f.mark(0, userRect(60, 680, 140, 720)));
        CHECK(hasCode(applyRedactions(*f.session, {}), ErrorCode::Unsupported));
    }
    {
        // A read-only field that holds nothing has nothing to remove.
        RedactionFixture f(formWith(pdf::PdfFormFieldType::Text, "", /*readOnly=*/true));
        CHECK(f.ready());
        CHECK(f.mark(0, userRect(60, 680, 140, 720)));
        CHECK(applyRedactions(*f.session, {}).has_value());
    }
}

RIVET_TEST(Redaction_apply_clears_defaults_or_refuses_when_they_cannot_be_removed) {
    {
        // The widget's own /DV or /RV: the value to apply also empties them.
        RedactionFixture f(formWith(pdf::PdfFormFieldType::Text, "classified", false, /*residue=*/true));
        CHECK(f.ready());
        CHECK(f.mark(0, userRect(60, 680, 140, 720)));
        auto command = applyRedactions(*f.session, {});
        CHECK(command.has_value());
        if (command.has_value()) CHECK(f.session->execute(std::move(*command)).has_value());
        const auto& values = f.session->pageSnapshot()->formValues();
        CHECK(values != nullptr && values->contains("secret") && values->at("secret").clearDefaults);
    }
    {
        // An already empty field still gets its default removed.
        RedactionFixture f(formWith(pdf::PdfFormFieldType::Text, "", false, /*residue=*/true));
        CHECK(f.ready());
        CHECK(f.mark(0, userRect(60, 680, 140, 720)));
        auto command = applyRedactions(*f.session, {});
        CHECK(command.has_value());
        if (command.has_value()) CHECK(f.session->execute(std::move(*command)).has_value());
        const auto& values = f.session->pageSnapshot()->formValues();
        CHECK(values != nullptr && values->contains("secret"));
    }
    {
        // A separate parent field may hold an unreachable /DV: refused.
        RedactionFixture f(formWith(pdf::PdfFormFieldType::Text, "classified", false, false, /*parent=*/true));
        CHECK(f.ready());
        CHECK(f.mark(0, userRect(60, 680, 140, 720)));
        CHECK(hasCode(applyRedactions(*f.session, {}), ErrorCode::Unsupported));
    }
}

RIVET_TEST(Redaction_apply_refuses_pages_with_unlisted_annotations) {
    RedactionFixture f;
    f.document->setAnnotations(0, {opaqueAt(0, 70, 695, 90, 705)}, 5000);
    CHECK(f.ready());
    CHECK(f.mark(0, userRect(60, 680, 140, 720)));
    CHECK(hasCode(applyRedactions(*f.session, {}), ErrorCode::Unsupported));
}

RIVET_TEST(Redaction_apply_refuses_pages_with_an_embedded_thumbnail) {
    RedactionFixture f;
    pdf::PdfPageContent content = samplePage();
    content.hasEmbeddedThumbnail = true;
    f.setContent(1, content);
    CHECK(f.ready(0));
    CHECK(f.ready(1));
    CHECK(f.mark(0, userRect(60, 680, 140, 720)));
    CHECK(f.mark(1, userRect(60, 680, 140, 720)));
    const auto blocker = redactionBlocker(*f.session, f.id(1));
    CHECK(blocker.has_value());
    if (blocker.has_value()) {
        CHECK(blocker->message.find("embedded thumbnail Rivet cannot remove") != std::string::npos);
    }
    CHECK(!redactionBlocker(*f.session, f.id(0)).has_value());
    CHECK(hasCode(applyRedactions(*f.session, {}), ErrorCode::Unsupported));
    CHECK(f.entry(0).contentEdits == nullptr);
}

RIVET_TEST(Redaction_apply_refuses_documents_with_xfa) {
    auto info = std::make_shared<pdf::PdfFormInfo>();
    info->hasXfa = true;
    RedactionFixture f(info);
    CHECK(f.ready());
    CHECK(f.mark(0, userRect(60, 680, 140, 720)));
    CHECK(redactionBlocker(*f.session, f.id(0)).has_value());
    auto command = applyRedactions(*f.session, {});
    CHECK(hasCode(command, ErrorCode::Unsupported));
    if (!command.has_value()) CHECK(command.error().message.find("XFA") != std::string::npos);
    CHECK(f.entry(0).contentEdits == nullptr);
}

RIVET_TEST(Redaction_apply_removes_annotations_with_non_finite_rects) {
    RedactionFixture f;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    f.document->setAnnotations(0, {opaqueAt(0, nan, 695, 90, 705), opaqueAt(1, 300, 300, 320, 310)});
    CHECK(f.ready());
    CHECK(f.mark(0, userRect(60, 680, 140, 720)));
    CHECK(f.runCommand(applyRedactions(*f.session, {})));
    const PageEntry& page = f.entry(0);
    CHECK(page.annotations != nullptr);
    if (page.annotations != nullptr) CHECK(page.annotations->suppressed == std::vector<std::uint32_t>({0}));
}
