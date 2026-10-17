// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "PdfFixtures.hpp"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfForms.hpp"
#include "pdf/PdfSystem.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

// PDFium AcroForm adapter (ADR-0024): reading fields, filling them through
// the assembly (PreserveBase and duplicates/Extract), /V + /AP persistence,
// refusal of read-only/unknown fields, and that filling never mutates the
// live document. PDFium-ON bodies only; every body returns early without a
// backend.

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
namespace pdffix = rivet::test::pdffix;
using namespace rivet::pdf;

std::unique_ptr<PdfEngine> pdfiumEngine() {
    std::unique_ptr<PdfEngine> engine = createEngine();
    CHECK(engine != nullptr);
    if (!engine || !engine->isAvailable()) return nullptr;
    return engine;
}

fs::path uniqueTempPath() {
    static int counter = 0;
    return fs::temp_directory_path() /
           ("rivet-forms-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(counter++) + ".pdf");
}

std::unique_ptr<PdfDocument> openBytes(PdfEngine& engine, const char* data, std::size_t size) {
    const fs::path path = uniqueTempPath();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(data, static_cast<std::streamsize>(size));
        CHECK(out.good());
    }
    auto opened = engine.openDocument(path, {});
    std::error_code ignored;
    fs::remove(path, ignored); // the document reads through its open descriptor
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

std::unique_ptr<PdfDocument> openForm(PdfEngine& engine) {
    const std::string bytes = pdffix::formPdf();
    return openBytes(engine, bytes.data(), bytes.size());
}

class MemorySink final : public IPdfByteSink {
public:
    core::Status write(const void* data, std::size_t size) override {
        const auto* bytes = static_cast<const char*>(data);
        bytes_.insert(bytes_.end(), bytes, bytes + size);
        return core::ok();
    }
    const std::vector<char>& bytes() const { return bytes_; }

private:
    std::vector<char> bytes_;
};

PdfPageView nativeView(const PdfDocument& document, std::size_t page) {
    const auto info = document.pageInfo(page);
    CHECK(info.has_value());
    return info.has_value() ? info->view : PdfPageView{};
}

PdfAssemblyRequest saveRequest(const PdfDocument& document, std::vector<std::size_t> pages) {
    PdfAssemblyRequest request;
    request.mode = PdfAssemblyRequest::Mode::PreserveBase;
    request.base = &document;
    for (const std::size_t page : pages) {
        request.pages.push_back(PdfAssemblyPage{&document, page, nativeView(document, page), nullptr});
    }
    return request;
}

const PdfFormField* field(const PdfFormInfo& info, const std::string& name) {
    const std::size_t index = info.find(name);
    return index == PdfFormInfo::npos ? nullptr : &info.fields[index];
}

PdfFormInfoPtr readInfo(const PdfDocument& document) {
    auto info = document.formInfo();
    CHECK(info.has_value());
    return info.has_value() ? *info : nullptr;
}

// Dark (non-background) pixels in a display-space rect of a rendered page.
std::size_t darkPixels(PdfDocument& document, std::size_t page, const core::Rect& rect) {
    auto bitmap = document.renderPage(page, rect, 2.0);
    CHECK(bitmap.has_value());
    if (!bitmap.has_value()) return 0;
    std::size_t dark = 0;
    const std::byte* data = bitmap->data();
    for (std::uint32_t y = 0; y < bitmap->height(); ++y) {
        for (std::uint32_t x = 0; x < bitmap->width(); ++x) {
            const std::byte* px = data + y * bitmap->stride() + x * 4;
            const auto b = static_cast<unsigned>(px[0]);
            const auto g = static_cast<unsigned>(px[1]);
            const auto r = static_cast<unsigned>(px[2]);
            if (r < 128 && g < 128 && b < 128) ++dark;
        }
    }
    return dark;
}

std::vector<std::uint8_t> pixels(PdfDocument& document, std::size_t page, const core::Rect& rect) {
    auto bitmap = document.renderPage(page, rect, 2.0);
    CHECK(bitmap.has_value());
    if (!bitmap.has_value()) return {};
    const auto* begin = reinterpret_cast<const std::uint8_t*>(bitmap->data());
    return std::vector<std::uint8_t>(begin, begin + bitmap->sizeBytes());
}

// Display rect (300x400 page, native view) of a user-space rect.
core::Rect display(double left, double bottom, double right, double top) {
    return core::Rect{left, 400.0 - top, right - left, top - bottom};
}

PdfFormValues filledValues() {
    PdfFormValues values;
    values["name"] = PdfFormValue::ofText("Bob");
    values["notes"] = PdfFormValue::ofText("First\nSecond");
    values["agree"] = PdfFormValue::ofToggle("Yes");
    values["color"] = PdfFormValue::ofToggle("Blue");
    values["size"] = PdfFormValue::ofChoice({2});
    values["fruits"] = PdfFormValue::ofChoice({0});
    values["city"] = PdfFormValue::ofText("Rome");
    return values;
}

} // namespace

RIVET_TEST(forms_read_fixture) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openForm(*engine);
    if (!doc) return;
    const PdfFormInfoPtr info = readInfo(*doc);
    if (!info) return;
    CHECK(!info->hasXfa);
    CHECK(!info->limitsExceeded);
    CHECK(info->fillable());
    CHECK_EQ(info->fields.size(), std::size_t{8});

    const PdfFormField* name = field(*info, "name");
    CHECK(name != nullptr);
    if (name != nullptr) {
        CHECK(name->type == PdfFormFieldType::Text);
        CHECK_EQ(name->value, std::string("Alice"));
        CHECK(!name->multiline);
        CHECK(name->fillable());
        CHECK_NEAR(static_cast<double>(name->fontSize), 12.0, 0.01);
        CHECK_EQ(name->widgets.size(), std::size_t{1});
        if (!name->widgets.empty()) {
            CHECK_EQ(name->widgets[0].pageIndex, std::size_t{0});
            CHECK_EQ(name->widgets[0].annotIndex, std::uint32_t{0});
            CHECK_NEAR(name->widgets[0].rect.left, 20.0, 0.01);
            CHECK_NEAR(name->widgets[0].rect.top, 360.0, 0.01);
        }
    }
    const PdfFormField* notes = field(*info, "notes");
    CHECK(notes != nullptr && notes->multiline && notes->value == "Line one");

    const PdfFormField* agree = field(*info, "agree");
    CHECK(agree != nullptr);
    if (agree != nullptr) {
        CHECK(agree->type == PdfFormFieldType::CheckBox);
        CHECK_EQ(agree->widgets.size(), std::size_t{1});
        if (!agree->widgets.empty()) {
            CHECK_EQ(agree->widgets[0].onState, std::string("Yes"));
            CHECK(!agree->widgets[0].checked);
        }
        CHECK(originalValue(*agree) == PdfFormValue::ofToggle({}));
    }
    const PdfFormField* color = field(*info, "color");
    CHECK(color != nullptr);
    if (color != nullptr) {
        CHECK(color->type == PdfFormFieldType::RadioButton);
        CHECK_EQ(color->widgets.size(), std::size_t{2});
        if (color->widgets.size() == 2) {
            CHECK_EQ(color->widgets[0].onState, std::string("Red"));
            CHECK_EQ(color->widgets[1].onState, std::string("Blue"));
            CHECK_EQ(color->widgets[0].annotIndex, std::uint32_t{3});
            CHECK_EQ(color->widgets[1].annotIndex, std::uint32_t{4});
        }
    }
    const PdfFormField* size = field(*info, "size");
    CHECK(size != nullptr);
    if (size != nullptr) {
        CHECK(size->type == PdfFormFieldType::ComboBox);
        CHECK_EQ(size->options.size(), std::size_t{3});
        if (size->options.size() == 3) {
            CHECK_EQ(size->options[1].label, std::string("Medium"));
            CHECK(size->options[0].selected);
        }
        CHECK(originalValue(*size) == PdfFormValue::ofChoice({0}));
    }
    const PdfFormField* fruits = field(*info, "fruits");
    CHECK(fruits != nullptr && fruits->type == PdfFormFieldType::ListBox && !fruits->multiSelect);
    if (fruits != nullptr) CHECK(originalValue(*fruits) == PdfFormValue::ofChoice({1}));

    const PdfFormField* id = field(*info, "id");
    CHECK(id != nullptr && id->readOnly && !id->fillable());

    const PdfFormField* city = field(*info, "city");
    CHECK(city != nullptr);
    if (city != nullptr) {
        CHECK_EQ(city->value, std::string("Paris"));
        CHECK_EQ(city->widgets.size(), std::size_t{2});
        if (city->widgets.size() == 2) {
            CHECK_EQ(city->widgets[0].pageIndex, std::size_t{0});
            CHECK_EQ(city->widgets[1].pageIndex, std::size_t{1});
            CHECK_EQ(city->widgets[1].annotIndex, std::uint32_t{0});
        }
    }
}

RIVET_TEST(forms_document_without_form_is_empty) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const std::string bytes = pdffix::richPdf();
    auto doc = openBytes(*engine, bytes.data(), bytes.size());
    if (!doc) return;
    const PdfFormInfoPtr info = readInfo(*doc);
    CHECK(info != nullptr && info->empty());
}

RIVET_TEST(forms_fill_save_reopen_persists_values_and_appearances) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openForm(*engine);
    if (!doc) return;
    const core::Rect nameRect = display(20, 340, 200, 360);
    // Widgets are drawn by the form filler: "name" (no /AP in the file) shows
    // its stored value through a generated appearance.
    CHECK_GT(darkPixels(*doc, 0, nameRect), std::size_t{0});
    const std::vector<std::uint8_t> nameBefore = pixels(*doc, 0, nameRect);
    const std::vector<std::uint8_t> liveBefore = pixels(*doc, 0, display(0, 0, 300, 400));

    PdfAssemblyRequest request = saveRequest(*doc, {0, 1});
    request.formValues = std::make_shared<const PdfFormValues>(filledValues());
    std::vector<std::string> failures;
    request.formFailures = &failures;
    MemorySink sink;
    const core::Status status = engine->assembleDocument(request, sink);
    CHECK(status.has_value());
    CHECK(failures.empty());
    if (!status.has_value()) return;

    // The live document is untouched: same pixels, same form description.
    CHECK(pixels(*doc, 0, display(0, 0, 300, 400)) == liveBefore);
    const PdfFormInfoPtr liveInfo = readInfo(*doc);
    CHECK(liveInfo != nullptr && field(*liveInfo, "name") != nullptr && field(*liveInfo, "name")->value == "Alice");

    auto saved = openBytes(*engine, sink.bytes().data(), sink.bytes().size());
    if (!saved) return;
    const PdfFormInfoPtr info = readInfo(*saved);
    if (!info) return;
    CHECK_EQ(info->fields.size(), std::size_t{8});
    const PdfFormField* name = field(*info, "name");
    CHECK(name != nullptr && name->value == "Bob");
    const PdfFormField* notes = field(*info, "notes");
    CHECK(notes != nullptr && notes->value == "First\nSecond");
    const PdfFormField* agree = field(*info, "agree");
    CHECK(agree != nullptr && originalValue(*agree) == PdfFormValue::ofToggle("Yes"));
    const PdfFormField* color = field(*info, "color");
    CHECK(color != nullptr && originalValue(*color) == PdfFormValue::ofToggle("Blue"));
    const PdfFormField* size = field(*info, "size");
    CHECK(size != nullptr && originalValue(*size) == PdfFormValue::ofChoice({2}));
    const PdfFormField* fruits = field(*info, "fruits");
    CHECK(fruits != nullptr && originalValue(*fruits) == PdfFormValue::ofChoice({0}));
    const PdfFormField* city = field(*info, "city");
    CHECK(city != nullptr && city->value == "Rome");
    const PdfFormField* id = field(*info, "id");
    CHECK(id != nullptr && id->value == "X-1");

    // /AP was regenerated: the filled text field now draws its value, and
    // the second "city" widget (page 2) shows the same value.
    CHECK_GT(darkPixels(*saved, 0, nameRect), std::size_t{0});
    CHECK(pixels(*saved, 0, nameRect) != nameBefore);
    CHECK_GT(darkPixels(*saved, 1, display(20, 340, 200, 360)), std::size_t{0});
    // The check box draws its on appearance.
    CHECK_GT(darkPixels(*saved, 0, display(20, 230, 34, 244)), std::size_t{0});
}

RIVET_TEST(forms_fill_reaches_duplicated_pages) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openForm(*engine);
    if (!doc) return;
    // Page 2 presented twice: the reused original and an imported duplicate.
    PdfAssemblyRequest request = saveRequest(*doc, {0, 1, 1});
    PdfFormValues values;
    values["city"] = PdfFormValue::ofText("Rome");
    request.formValues = std::make_shared<const PdfFormValues>(values);
    MemorySink sink;
    CHECK(engine->assembleDocument(request, sink).has_value());
    auto saved = openBytes(*engine, sink.bytes().data(), sink.bytes().size());
    if (!saved) return;
    const core::Rect cityRect = display(20, 340, 200, 360);
    const std::vector<std::uint8_t> original = pixels(*saved, 1, cityRect);
    CHECK(!original.empty());
    CHECK(pixels(*saved, 2, cityRect) == original);
    CHECK_GT(darkPixels(*saved, 2, cityRect), std::size_t{0});
}

RIVET_TEST(forms_fill_with_deleted_widget_page) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openForm(*engine);
    if (!doc) return;
    // Page 1 deleted: "city" still has its page-2 widget; "name" (page 1
    // only) has none and is skipped.
    PdfAssemblyRequest request = saveRequest(*doc, {1});
    PdfFormValues values;
    values["city"] = PdfFormValue::ofText("Rome");
    values["name"] = PdfFormValue::ofText("Bob");
    request.formValues = std::make_shared<const PdfFormValues>(values);
    MemorySink sink;
    CHECK(engine->assembleDocument(request, sink).has_value());
    auto saved = openBytes(*engine, sink.bytes().data(), sink.bytes().size());
    if (!saved) return;
    const PdfFormInfoPtr info = readInfo(*saved);
    if (!info) return;
    const PdfFormField* city = field(*info, "city");
    CHECK(city != nullptr && city->value == "Rome");
}

RIVET_TEST(forms_fill_refuses_read_only_unknown_and_invalid) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openForm(*engine);
    if (!doc) return;
    const auto attempt = [&](const std::string& name, PdfFormValue value) {
        PdfAssemblyRequest request = saveRequest(*doc, {0, 1});
        PdfFormValues values;
        values[name] = std::move(value);
        request.formValues = std::make_shared<const PdfFormValues>(values);
        MemorySink sink;
        const core::Status status = engine->assembleDocument(request, sink);
        return status.has_value() ? core::ErrorCode::None : status.error().code;
    };
    CHECK(attempt("id", PdfFormValue::ofText("X-2")) == core::ErrorCode::InvalidArgument);
    CHECK(attempt("missing", PdfFormValue::ofText("x")) == core::ErrorCode::InvalidArgument);
    CHECK(attempt("name", PdfFormValue::ofToggle("Yes")) == core::ErrorCode::InvalidArgument);
    CHECK(attempt("name", PdfFormValue::ofText("a\nb")) == core::ErrorCode::InvalidArgument);
    CHECK(attempt("color", PdfFormValue::ofToggle("Green")) == core::ErrorCode::InvalidArgument);
    CHECK(attempt("size", PdfFormValue::ofChoice({7})) == core::ErrorCode::InvalidArgument);
    CHECK(attempt("fruits", PdfFormValue::ofChoice({0, 1})) == core::ErrorCode::InvalidArgument);
}

RIVET_TEST(forms_extract_with_values_succeeds) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openForm(*engine);
    if (!doc) return;
    // Extract (Fresh): no AcroForm is carried over; the imported copy of the
    // base page was filled first, so its widget keeps the filled /AP.
    PdfAssemblyRequest request = saveRequest(*doc, {1});
    request.mode = PdfAssemblyRequest::Mode::Fresh;
    PdfFormValues values;
    values["city"] = PdfFormValue::ofText("Rome");
    request.formValues = std::make_shared<const PdfFormValues>(values);
    MemorySink sink;
    CHECK(engine->assembleDocument(request, sink).has_value());
    auto extracted = openBytes(*engine, sink.bytes().data(), sink.bytes().size());
    CHECK(extracted != nullptr);
    if (extracted == nullptr) return;
    CHECK_EQ(extracted->info().pageCount, std::size_t{1});
    const PdfFormInfoPtr info = readInfo(*extracted);
    CHECK(info != nullptr && info->empty());
}

// QA #7: /DV is read from the widget and a Reset
// Form value set saves cleanly: text fields return to /DV or empty, the list
// box to no selection; the radio group and the combo box (no /DV) keep their
// stored values.
RIVET_TEST(forms_reset_values_read_defaults_and_save) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openForm(*engine);
    if (!doc) return;
    PdfAssemblyRequest request = saveRequest(*doc, {0, 1});
    request.formValues = std::make_shared<const PdfFormValues>(filledValues());
    MemorySink sink;
    CHECK(engine->assembleDocument(request, sink).has_value());
    auto filled = openBytes(*engine, sink.bytes().data(), sink.bytes().size());
    if (!filled) return;
    const PdfFormInfoPtr info = readInfo(*filled);
    if (!info) return;
    const PdfFormField* name = field(*info, "name");
    CHECK(name != nullptr && name->defaultValue == std::optional<std::string>("Your name"));
    const PdfFormField* notes = field(*info, "notes");
    CHECK(notes != nullptr && !notes->defaultValue.has_value());

    const PdfFormValues reset = resetFormValues(*info);
    CHECK(!reset.empty());
    PdfAssemblyRequest resetRequest = saveRequest(*filled, {0, 1});
    resetRequest.formValues = std::make_shared<const PdfFormValues>(reset);
    std::vector<std::string> failures;
    resetRequest.formFailures = &failures;
    MemorySink out;
    CHECK(engine->assembleDocument(resetRequest, out).has_value());
    CHECK(failures.empty());
    auto saved = openBytes(*engine, out.bytes().data(), out.bytes().size());
    if (!saved) return;
    const PdfFormInfoPtr after = readInfo(*saved);
    if (!after) return;
    CHECK(field(*after, "name") != nullptr && field(*after, "name")->value == "Your name");
    CHECK(field(*after, "city") != nullptr && field(*after, "city")->value.empty());
    CHECK(field(*after, "notes") != nullptr && field(*after, "notes")->value.empty());
    CHECK(field(*after, "agree") != nullptr && originalValue(*field(*after, "agree")) == PdfFormValue::ofToggle(""));
    CHECK(field(*after, "fruits") != nullptr &&
          originalValue(*field(*after, "fruits")) == PdfFormValue::ofChoice({}));
    CHECK(field(*after, "color") != nullptr &&
          originalValue(*field(*after, "color")) == PdfFormValue::ofToggle("Blue"));
    CHECK(field(*after, "size") != nullptr && originalValue(*field(*after, "size")) == PdfFormValue::ofChoice({2}));
    CHECK(resetFormValues(*after).empty()); // nothing left to reset
}
