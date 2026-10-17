// SPDX-License-Identifier: MPL-2.0
// AcroForm values in the editor (ADR-0024), portable (fakes): the form
// service (lazy description, widgets in display space, hidden widget sets),
// FormValuesCommand (document-level values + raster revisions, undo/redo,
// reset, duplicates) and the save request.
#include "RivetTest.h"

#include "fakes/AnnotationTestSupport.hpp"

#include "editor/FormCommands.hpp"
#include "editor/FormService.hpp"
#include "editor/PageCommands.hpp"
#include "pdf/PdfForms.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace rivet;
using namespace rivet::editor;
using namespace rivet::test;

pdf::PdfFormWidget widgetAt(std::size_t page, std::uint32_t annot, pdf::PdfBox rect, std::string onState = {}) {
    pdf::PdfFormWidget w;
    w.pageIndex = page;
    w.annotIndex = annot;
    w.rect = rect;
    w.onState = std::move(onState);
    return w;
}

// "a": text with widgets on pages 0 (/Annots 2) and 1 (/Annots 0); "b": check
// box on page 2 (/Annots 1, on state Yes); "ro": read-only text on page 0
// (/Annots 0).
pdf::PdfFormInfoPtr sampleForm() {
    auto info = std::make_shared<pdf::PdfFormInfo>();
    pdf::PdfFormField a;
    a.name = "a";
    a.type = pdf::PdfFormFieldType::Text;
    a.value = "orig";
    a.widgets = {widgetAt(0, 2, {100, 700, 300, 720}), widgetAt(1, 0, {100, 600, 300, 620})};
    pdf::PdfFormField b;
    b.name = "b";
    b.type = pdf::PdfFormFieldType::CheckBox;
    b.value = "Off";
    b.widgets = {widgetAt(2, 1, {50, 50, 64, 64}, "Yes")};
    pdf::PdfFormField ro;
    ro.name = "ro";
    ro.type = pdf::PdfFormFieldType::Text;
    ro.readOnly = true;
    ro.widgets = {widgetAt(0, 0, {10, 10, 60, 30})};
    info->fields = {std::move(a), std::move(b), std::move(ro)};
    return info;
}

struct FormFixture : AnnotationFixture {
    explicit FormFixture(pdf::PdfFormInfoPtr info = sampleForm()) : AnnotationFixture(3) {
        document->setFormInfo(std::move(info));
        session->forms().ensureLoaded();
        CHECK(settle(dispatcher, [&] { return session->forms().state() == FormService::State::Ready; }));
    }

    bool set(const std::string& name, pdf::PdfFormValue value) {
        auto command = makeSetFormValue(session->pageModel(), *session->forms().info(), name, std::move(value));
        if (!command.has_value() || *command == nullptr) return false;
        return session->execute(std::move(*command)).has_value();
    }

    std::uint64_t raster(std::size_t index) const { return entry(index).rasterRevision; }
    std::vector<std::uint32_t> hidden(std::size_t index) const {
        return session->forms().hiddenWidgets(*session->pageSnapshot(), entry(index));
    }
};

} // namespace

RIVET_TEST(forms_service_loads_lazily_and_lists_widgets) {
    FormFixture f;
    const pdf::PdfFormInfoPtr info = f.session->forms().info();
    CHECK(info != nullptr && info->fields.size() == 3);
    const std::vector<FormWidgetView> page0 = f.session->forms().widgets(f.id(0));
    CHECK_EQ(page0.size(), std::size_t{2});
    if (page0.size() == 2) {
        // /Annots order: "ro" (0) before "a" (2).
        CHECK_EQ(page0[0].field, std::size_t{2});
        CHECK_EQ(page0[1].field, std::size_t{0});
        // Display space of a 612x792 page: y = 792 - top.
        CHECK_NEAR(page0[1].displayRect.origin.x, 100.0, 1e-9);
        CHECK_NEAR(page0[1].displayRect.origin.y, 72.0, 1e-9);
        CHECK_NEAR(page0[1].displayRect.size.width, 200.0, 1e-9);
        CHECK(!page0[1].edited);
    }
    CHECK_EQ(f.session->forms().widgets(f.id(2)).size(), std::size_t{1});
    CHECK(f.session->forms().currentValue(0) == pdf::PdfFormValue::ofText("orig"));
    CHECK(f.session->forms().currentValue(1) == pdf::PdfFormValue::ofToggle({}));
}

RIVET_TEST(forms_backend_without_support_is_empty) {
    FormFixture f(nullptr);
    const pdf::PdfFormInfoPtr info = f.session->forms().info();
    CHECK(info != nullptr && info->empty());
    CHECK(f.session->forms().widgets(f.id(0)).empty());
}

RIVET_TEST(forms_set_value_swaps_values_and_rasters_with_exact_undo) {
    FormFixture f;
    const std::uint64_t r0 = f.raster(0);
    const std::uint64_t r1 = f.raster(1);
    const std::uint64_t r2 = f.raster(2);
    CHECK(!f.session->isDirty());

    CHECK(f.set("a", pdf::PdfFormValue::ofText("new")));
    CHECK(f.session->isDirty());
    const pdf::PdfFormValuesPtr values = f.session->pageSnapshot()->formValues();
    CHECK(values != nullptr && values->size() == 1 && values->at("a") == pdf::PdfFormValue::ofText("new"));
    CHECK(f.raster(0) != r0);
    CHECK(f.raster(1) != r1);
    CHECK_EQ(f.raster(2), r2); // no widget of "a" there
    CHECK(f.hidden(0) == std::vector<std::uint32_t>{2});
    CHECK(f.hidden(1) == std::vector<std::uint32_t>{0});
    CHECK(f.hidden(2).empty());
    CHECK(f.session->forms().currentValue(0) == pdf::PdfFormValue::ofText("new"));
    const std::vector<FormWidgetView> page0 = f.session->forms().widgets(f.id(0));
    CHECK(page0.size() == 2 && page0[1].edited && !page0[0].edited);
    const std::uint64_t after0 = f.raster(0);

    CHECK(f.session->undo());
    CHECK(f.session->pageSnapshot()->formValues() == nullptr);
    CHECK_EQ(f.raster(0), r0);
    CHECK_EQ(f.raster(1), r1);
    CHECK(f.hidden(0).empty());
    CHECK(!f.session->isDirty());

    CHECK(f.session->redo());
    CHECK(f.session->pageSnapshot()->formValues() == values);
    CHECK_EQ(f.raster(0), after0);
}

RIVET_TEST(forms_value_equal_to_stored_drops_the_edit) {
    FormFixture f;
    // Nothing edited, same value: no command.
    auto none = makeSetFormValue(f.session->pageModel(), *f.session->forms().info(), "a",
                                 pdf::PdfFormValue::ofText("orig"));
    CHECK(none.has_value() && *none == nullptr);
    CHECK(f.set("b", pdf::PdfFormValue::ofToggle("Yes")));
    CHECK(f.hidden(2) == std::vector<std::uint32_t>{1});
    // Back to the stored "off": the entry disappears, the overlay too.
    CHECK(f.set("b", pdf::PdfFormValue::ofToggle({})));
    CHECK(f.session->pageSnapshot()->formValues() == nullptr);
    CHECK(f.hidden(2).empty());
}

RIVET_TEST(forms_refuse_read_only_unknown_and_invalid) {
    FormFixture f;
    const pdf::PdfFormInfo& info = *f.session->forms().info();
    PageModel& model = f.session->pageModel();
    CHECK(!makeSetFormValue(model, info, "ro", pdf::PdfFormValue::ofText("x")).has_value());
    CHECK(!makeSetFormValue(model, info, "zz", pdf::PdfFormValue::ofText("x")).has_value());
    CHECK(!makeSetFormValue(model, info, "b", pdf::PdfFormValue::ofToggle("No")).has_value());
    CHECK(!makeSetFormValue(model, info, "a", pdf::PdfFormValue::ofText("two\nlines")).has_value());
    pdf::PdfFormInfo xfa = info;
    xfa.hasXfa = true;
    CHECK(!makeSetFormValue(model, xfa, "a", pdf::PdfFormValue::ofText("x")).has_value());
}

RIVET_TEST(forms_reset_sets_defaults_as_one_step) {
    FormFixture f;
    const std::uint64_t r2 = f.raster(2);
    CHECK(f.set("a", pdf::PdfFormValue::ofText("1")));
    CHECK(f.set("b", pdf::PdfFormValue::ofToggle("Yes")));
    const pdf::PdfFormValuesPtr edited = f.session->pageSnapshot()->formValues();
    CHECK(edited != nullptr && edited->size() == 2);

    // Defaults (no /DV): "a" empty (its stored "orig" is replaced), "b" off
    // (= stored, so its edit is dropped and the raster draws it again).
    auto reset = makeResetForm(f.session->pageModel(), *f.session->forms().info());
    CHECK(reset.has_value() && *reset != nullptr);
    CHECK(f.session->execute(std::move(*reset)).has_value());
    const pdf::PdfFormValuesPtr after = f.session->pageSnapshot()->formValues();
    CHECK(after != nullptr && after->size() == 1);
    CHECK(after != nullptr && after->contains("a") && after->at("a") == pdf::PdfFormValue::ofText(""));
    CHECK(f.hidden(2).empty());
    CHECK(!f.hidden(0).empty());
    CHECK(f.raster(2) != r2);
    // Already at the defaults: no further reset.
    auto again = makeResetForm(f.session->pageModel(), *f.session->forms().info());
    CHECK(again.has_value() && *again == nullptr);

    CHECK(f.session->undo());
    CHECK(f.session->pageSnapshot()->formValues() == edited);
    // QA #7: nothing edited, but the stored "orig" is not the default: a
    // freshly opened filled form can be reset.
    CHECK(f.session->undo());
    CHECK(f.session->undo());
    auto fresh = makeResetForm(f.session->pageModel(), *f.session->forms().info());
    CHECK(fresh.has_value() && *fresh != nullptr);
}

RIVET_TEST(forms_duplicated_pages_follow_the_field) {
    FormFixture f;
    auto duplicate = std::make_unique<DuplicatePagesCommand>(f.session->pageModel(), std::vector<core::PageId>{f.id(1)});
    auto* raw = duplicate.get();
    CHECK(f.session->execute(std::move(duplicate)).has_value());
    CHECK_EQ(raw->createdIds().size(), std::size_t{1});
    if (raw->createdIds().empty()) return;
    const core::PageId copy = raw->createdIds()[0];
    const PageEntry* before = f.session->pageSnapshot()->find(copy);
    CHECK(before != nullptr);
    if (before == nullptr) return;
    const std::uint64_t copyRaster = before->rasterRevision;

    CHECK(f.set("a", pdf::PdfFormValue::ofText("dup")));
    const PageModelSnapshot& snapshot = *f.session->pageSnapshot();
    const PageEntry* after = snapshot.find(copy);
    CHECK(after != nullptr && after->rasterRevision != copyRaster);
    if (after != nullptr) CHECK(f.session->forms().hiddenWidgets(snapshot, *after) == std::vector<std::uint32_t>{0});
    CHECK_EQ(f.session->forms().widgets(copy).size(), std::size_t{1});
}

RIVET_TEST(forms_values_reach_the_save_and_extract_requests) {
    FormFixture f;
    CHECK(f.set("a", pdf::PdfFormValue::ofText("saved")));
    const PageSnapshotPtr snapshot = f.session->pageSnapshot();
    auto save = snapshot->toAssemblyRequest(PageModelSnapshot::AssemblyMode::Save);
    CHECK(save.has_value() && save->formValues == snapshot->formValues());
    const std::vector<core::PageId> subset{f.id(1)};
    auto extract = snapshot->toAssemblyRequest(PageModelSnapshot::AssemblyMode::Extract, subset);
    CHECK(extract.has_value() && extract->formValues == snapshot->formValues());
    CHECK(f.session->undo());
    auto clean = f.session->pageSnapshot()->toAssemblyRequest(PageModelSnapshot::AssemblyMode::Save);
    CHECK(clean.has_value() && clean->formValues == nullptr);
}

RIVET_TEST(forms_render_target_hides_edited_widgets) {
    FormFixture f;
    CHECK(f.set("a", pdf::PdfFormValue::ofText("x")));
    // Render page 0 through the session: the edited widget is hidden.
    const PageEntry& entry = f.entry(0);
    render::RenderRequest r;
    r.key.documentId = f.session->id();
    r.key.pageId = f.id(0);
    r.key.scale = render::PhysicalRenderScaleKey::fromDensities(0.25, 1.0);
    r.key.contentRevision = entry.rasterRevision;
    const core::Size size = pdf::displaySize(entry.view);
    r.params.pageRectPoints = core::Rect{0.0, 0.0, size.width, size.height};
    r.params.devicePixelsPerPoint = r.key.scale.scale();
    auto done = std::make_shared<std::atomic<int>>(0);
    f.session->renderSource().requestRender(r, render::RenderPriority::Visible, [done](render::RenderResult result) {
        done->store(result.has_value() ? 1 : 2);
    });
    CHECK(settle(f.dispatcher, [&] { return done->load() != 0; }));
    CHECK_EQ(done->load(), 1);
    CHECK(f.document->lastHiddenAnnotations() == std::vector<std::uint32_t>{2});
}
