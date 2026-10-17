// SPDX-License-Identifier: MPL-2.0
// AcroForm filling in the app layer (ADR-0024): the pure layout helpers
// (masking, font size, comb/multiline layout, traversal order) and the
// FormController over a real DocumentSession (fake page backend with a form
// description): press -> editor / toggle / popup, one undo step per change,
// Tab traversal, Esc priorities, read-only/XFA status reasons, Reset Form,
// Highlight Form Fields and the menu entries. Deterministic: the test thread
// is the main thread and only blocks on the dispatcher queue.
#include "RivetTest.h"

#include "Fakes.hpp"
#include "fakes/FakePageDocument.hpp"

#include "app/AnnotationController.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/FormController.hpp"
#include "app/FormLayout.hpp"
#include "app/MenuModel.hpp"
#include "app/ShellContext.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/FormService.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TextArea.hpp"
#include "ui/TextField.hpp"
#include "ui/UiTypes.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace rivet;
using app::AnnotationController;
using app::AnnotationTool;
using app::AppCommand;
using app::DocumentTab;
using app::DocumentWorkspace;
using app::FormCommand;
using app::FormController;
using app::ShellContext;

namespace {

// --- Pure layout helpers -------------------------------------------------------

double fixedWidth(std::string_view text, double size) {
    return 0.5 * size * static_cast<double>(text.size());
}

} // namespace

RIVET_TEST(formLayoutMasksPerCodePointAndSizesText) {
    CHECK_EQ(app::maskFormText("ab\xC3\xA9"), std::string("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"));
    CHECK(app::maskFormText("").empty());

    pdf::PdfFormField field;
    field.type = pdf::PdfFormFieldType::Text;
    field.fontSize = 9.0F;
    const core::Rect rect{0.0, 0.0, 200.0, 20.0};
    CHECK_NEAR(app::formFontSize(field, rect, "x", fixedWidth), 9.0, 1e-9); // explicit size wins

    field.fontSize = 0.0F; // auto: 0.75 x inner height, shrunk to fit the width
    CHECK_NEAR(app::formFontSize(field, rect, "x", fixedWidth), 12.0, 1e-9);
    const double shrunk = app::formFontSize(field, rect, std::string(100, 'x'), fixedWidth);
    CHECK(shrunk < 12.0);
    CHECK(shrunk >= app::kFormMinFontSize);
}

RIVET_TEST(formLayoutCombCellsAndMultilineWrap) {
    pdf::PdfFormField comb;
    comb.type = pdf::PdfFormFieldType::Text;
    comb.comb = true;
    comb.maxLength = 4;
    const app::FormTextLayout cells = app::layoutFormText(comb, core::Rect{0.0, 0.0, 80.0, 20.0}, "123456", fixedWidth);
    CHECK_EQ(cells.lines.size(), std::size_t{4}); // capped at maxLength
    CHECK_NEAR(cells.lines[1].rect.origin.x, 20.0, 1e-9);
    CHECK(cells.lines[0].centered);

    pdf::PdfFormField area;
    area.type = pdf::PdfFormFieldType::Text;
    area.multiline = true;
    area.fontSize = 10.0F;
    // 5 px per character: "aaaa bbbb cccc" wraps at a 50 px inner width.
    const app::FormTextLayout wrapped =
        app::layoutFormText(area, core::Rect{0.0, 0.0, 54.0, 200.0}, "aaaa bbbb cccc\nd", fixedWidth);
    CHECK(wrapped.lines.size() >= 3);
    CHECK_EQ(wrapped.lines.back().text, std::string("d"));
    for (std::size_t i = 1; i < wrapped.lines.size(); ++i) {
        CHECK(wrapped.lines[i].rect.origin.y > wrapped.lines[i - 1].rect.origin.y);
    }
    // No room: lines past the bottom are dropped.
    const app::FormTextLayout clipped =
        app::layoutFormText(area, core::Rect{0.0, 0.0, 54.0, 14.0}, "a\nb\nc", fixedWidth);
    CHECK_EQ(clipped.lines.size(), std::size_t{1});
}

RIVET_TEST(formLayoutTraversalOrderWrapsBothWays) {
    std::vector<app::FormStop> stops = {
        {1, 0, 5, 0},
        {0, 3, 2, 0},
        {0, 1, 7, 0},
    };
    app::sortFormStops(stops);
    CHECK_EQ(stops[0].field, std::size_t{7});
    CHECK_EQ(stops[1].field, std::size_t{2});
    CHECK_EQ(stops[2].field, std::size_t{5});
    CHECK_EQ(app::nextFormStop(stops, std::nullopt, false).value(), std::size_t{0});
    CHECK_EQ(app::nextFormStop(stops, std::nullopt, true).value(), std::size_t{2});
    CHECK_EQ(app::nextFormStop(stops, std::size_t{2}, false).value(), std::size_t{0});
    CHECK_EQ(app::nextFormStop(stops, std::size_t{0}, true).value(), std::size_t{2});
    CHECK(!app::nextFormStop({}, std::nullopt, false).has_value());
}

RIVET_TEST(formLayoutChoiceTextJoinsSelectedLabels) {
    pdf::PdfFormField field;
    field.type = pdf::PdfFormFieldType::ListBox;
    field.options = {{"Apple", "Apple", false}, {"Pear", "Pear", false}, {"Plum", "Plum", false}};
    CHECK_EQ(app::formChoiceText(field, pdf::PdfFormValue::ofChoice({0, 2})), std::string("Apple, Plum"));
    CHECK_EQ(app::formChoiceText(field, pdf::PdfFormValue::ofChoice({9})), std::string());
}

RIVET_TEST(menuModelHasTheFormEntries) {
    const auto bar = app::buildMenuBar();
    bool reset = false;
    bool highlight = false;
    for (const app::Menu& menu : bar) {
        for (const app::MenuItem& item : menu.items) {
            if (item.command == AppCommand::of(FormCommand::ResetForm)) {
                reset = menu.title == "Edit" && item.title == "Reset Form";
            }
            if (item.command == AppCommand::of(FormCommand::HighlightFields)) {
                highlight = menu.title == "View" && item.checkable;
            }
        }
    }
    CHECK(reset);
    CHECK(highlight);
    CHECK(AppCommand::of(FormCommand::HighlightFields).form() == FormCommand::HighlightFields);
}

// --- Controller over a session ----------------------------------------------------

namespace {

class WaitDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_all();
    }
    void pump() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            run.swap(queue_);
        }
        for (auto& task : run) task();
    }
    bool waitUntil(const std::function<bool()>& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            pump();
            if (predicate()) return true;
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(30)),
                                [this] { return !queue_.empty(); })) {
                if (std::chrono::steady_clock::now() >= deadline) return predicate();
            }
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

std::filesystem::path tempPdf(const char* name) {
    auto path = std::filesystem::temp_directory_path() /
                std::filesystem::path{std::string{"rivet-formctl-"} + name + ".pdf"};
    if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
        std::fputs("%PDF-1.4\n", file);
        std::fclose(file);
    }
    return path;
}

pdf::PdfFormWidget widgetOn(std::size_t page, std::uint32_t annot, pdf::PdfBox rect, std::string onState = {}) {
    pdf::PdfFormWidget w;
    w.pageIndex = page;
    w.annotIndex = annot;
    w.rect = rect;
    w.onState = std::move(onState);
    return w;
}

// Pages are 612 x 792 (display y = 792 - user y). Fields:
//   0 "name"  text, page 0 /Annots 1, display {100, 72, 200, 20}
//   1 "agree" check box, page 0 /Annots 2, display {100, 142, 14, 14}
//   2 "color" radio Red (/Annots 3) / Blue (/Annots 4) on page 0
//   3 "size"  combo S/M/L, page 1 /Annots 0
//   4 "id"    read-only text, page 0 /Annots 0, display {10, 762, 50, 20}
//   5 "notes" multiline text, page 1 /Annots 1
pdf::PdfFormInfoPtr sampleForm() {
    auto info = std::make_shared<pdf::PdfFormInfo>();
    pdf::PdfFormField name;
    name.name = "name";
    name.type = pdf::PdfFormFieldType::Text;
    name.value = "orig";
    name.widgets = {widgetOn(0, 1, {100, 700, 300, 720})};
    pdf::PdfFormField agree;
    agree.name = "agree";
    agree.type = pdf::PdfFormFieldType::CheckBox;
    agree.value = "Off";
    agree.widgets = {widgetOn(0, 2, {100, 636, 114, 650}, "Yes")};
    pdf::PdfFormField color;
    color.name = "color";
    color.type = pdf::PdfFormFieldType::RadioButton;
    color.value = "Off";
    color.widgets = {widgetOn(0, 3, {100, 600, 114, 614}, "Red"), widgetOn(0, 4, {130, 600, 144, 614}, "Blue")};
    pdf::PdfFormField size;
    size.name = "size";
    size.type = pdf::PdfFormFieldType::ComboBox;
    size.options = {{"S", "S", false}, {"M", "M", true}, {"L", "L", false}};
    size.value = "M";
    size.widgets = {widgetOn(1, 0, {100, 700, 200, 720})};
    pdf::PdfFormField id;
    id.name = "id";
    id.type = pdf::PdfFormFieldType::Text;
    id.readOnly = true;
    id.value = "X-1";
    id.widgets = {widgetOn(0, 0, {10, 10, 60, 30})};
    pdf::PdfFormField notes;
    notes.name = "notes";
    notes.type = pdf::PdfFormFieldType::Text;
    notes.multiline = true;
    notes.widgets = {widgetOn(1, 1, {100, 500, 400, 600})};
    info->fields = {std::move(name), std::move(agree), std::move(color),
                    std::move(size), std::move(id),    std::move(notes)};
    return info;
}

ui::KeyEvent keyOf(ui::Key key, bool shift = false) {
    ui::KeyEvent event;
    event.key = key;
    event.modifiers.shift = shift;
    return event;
}

struct Shell {
    test::FakePageEngine engine{3};
    core::TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    DocumentWorkspace workspace{engine, scheduler, &dispatcher};
    platform::ShellServices services;

    std::unique_ptr<ui::Container> root = std::make_unique<ui::Container>();
    ui::PdfViewport* viewport = nullptr;
    ui::Widget* focused = nullptr;
    std::vector<std::string> statusLog;
    std::unique_ptr<ShellContext> context;
    std::unique_ptr<AnnotationController> annotations;
    std::unique_ptr<FormController> forms;

    explicit Shell(pdf::PdfFormInfoPtr info = sampleForm()) {
        engine.formInfo = std::move(info);
        services.mainDispatcher = &dispatcher;
        auto vp = std::make_unique<ui::PdfViewport>();
        viewport = vp.get();
        viewport->setFrame(core::Rect{180.0, 68.0, 800.0, 600.0});
        context = std::make_unique<ShellContext>(ShellContext{
            workspace,
            services,
            *viewport,
            [this](std::string message) { statusLog.push_back(std::move(message)); },
            [this](ui::Widget* widget) {
                if (focused == widget) return;
                if (focused != nullptr) focused->setFocused(false);
                focused = widget;
                if (focused != nullptr) focused->setFocused(true);
            },
            [] {},
        });
        root->addChild(std::move(vp));
        annotations = std::make_unique<AnnotationController>(*context, *root);
        forms = std::make_unique<FormController>(*context, *root, *annotations);
        forms->layout(viewport->frame());
        workspace.setOnActiveTabChanged([this] { bind(); });
    }

    ~Shell() {
        forms.reset();
        annotations.reset();
        viewport->clearDocument();
        workspace.setOnActiveTabChanged({});
    }

    void bind() {
        DocumentTab* tab = workspace.activeTab();
        if (tab == nullptr || tab->state() != DocumentTab::State::Ready) {
            viewport->clearDocument();
            annotations->bindTab(nullptr);
            forms->bindTab(nullptr);
            return;
        }
        editor::DocumentSession* session = tab->session();
        viewport->setDocument(session->id(), &session->layout(), &session->renderSource(),
                              [session] { return session->revision(); }, &tab->viewState());
        annotations->bindTab(tab);
        forms->bindTab(tab);
    }

    DocumentTab* open(const char* name) {
        const std::size_t before = workspace.tabCount();
        workspace.openDocument(tempPdf(name));
        if (!dispatcher.waitUntil([this, before] {
                DocumentTab* tab = workspace.activeTab();
                return workspace.tabCount() > before && tab != nullptr && tab->state() == DocumentTab::State::Ready &&
                       tab->session()->forms().state() == editor::FormService::State::Ready;
            })) {
            return nullptr;
        }
        return workspace.activeTab();
    }

    editor::DocumentSession& session() { return *workspace.activeTab()->session(); }
    pdf::PdfFormValuesPtr values() { return session().pageSnapshot()->formValues(); }
    std::optional<pdf::PdfFormValue> valueOf(const char* field) {
        const pdf::PdfFormValuesPtr v = values();
        if (v == nullptr) return std::nullopt;
        const auto it = v->find(field);
        if (it == v->end()) return std::nullopt;
        return it->second;
    }
    std::string lastStatus() const { return statusLog.empty() ? std::string() : statusLog.back(); }
    bool statusContains(std::string_view needle) const {
        for (const std::string& line : statusLog) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

constexpr core::Point kNameCenter{200.0, 82.0};
constexpr core::Point kAgreeCenter{107.0, 149.0};
constexpr core::Point kRedCenter{107.0, 185.0};
constexpr core::Point kBlueCenter{137.0, 185.0};
constexpr core::Point kSizeCenter{150.0, 82.0};
constexpr core::Point kIdCenter{35.0, 772.0};
constexpr core::Point kNotesCenter{250.0, 242.0};

} // namespace

RIVET_TEST(formControllerTextEditorCommitsOneUndoStep) {
    Shell shell;
    CHECK(shell.open("text") != nullptr);
    FormController& forms = *shell.forms;
    CHECK(forms.interactive());

    CHECK(forms.press(0, kNameCenter));
    CHECK(forms.editorOpen());
    CHECK(forms.focus().has_value());
    CHECK_EQ(forms.focus()->field, std::size_t{0});
    CHECK(shell.focused == &forms.lineEditor());
    CHECK_EQ(forms.lineEditor().text(), std::string("orig"));

    forms.lineEditor().setText("Ada Lovelace");
    CHECK(forms.commitEditor());
    CHECK(!forms.editorOpen());
    CHECK(shell.valueOf("name").has_value());
    CHECK_EQ(shell.valueOf("name")->text, std::string("Ada Lovelace"));
    CHECK(shell.session().isDirty());

    // One undo step restores the stored value (the edit disappears).
    CHECK(shell.session().undo());
    CHECK(!shell.valueOf("name").has_value());
    CHECK(shell.session().redo());
    CHECK_EQ(shell.valueOf("name")->text, std::string("Ada Lovelace"));

    // An unchanged commit runs no command; Escape cancels without one.
    CHECK(forms.press(0, kNameCenter));
    const auto before = shell.session().commands().stateId();
    CHECK(forms.commitEditor());
    CHECK(forms.press(0, kNameCenter));
    forms.lineEditor().setText("discarded");
    CHECK(forms.handleEscape());
    CHECK(!forms.editorOpen());
    CHECK_EQ(shell.session().commands().stateId(), before);
    CHECK_EQ(shell.valueOf("name")->text, std::string("Ada Lovelace"));
}

RIVET_TEST(formControllerMultilineUsesTheTextArea) {
    Shell shell;
    CHECK(shell.open("area") != nullptr);
    FormController& forms = *shell.forms;
    CHECK(forms.press(1, kNotesCenter));
    CHECK(forms.editorOpen());
    CHECK(shell.focused == &forms.areaEditor());
    forms.areaEditor().setText("line one\nline two");
    CHECK(forms.commitEditor());
    CHECK_EQ(shell.valueOf("notes")->text, std::string("line one\nline two"));
}

RIVET_TEST(formControllerTogglesCheckBoxesAndRadios) {
    Shell shell;
    CHECK(shell.open("toggle") != nullptr);
    FormController& forms = *shell.forms;

    CHECK(forms.press(0, kAgreeCenter));
    CHECK_EQ(shell.valueOf("agree")->text, std::string("Yes"));
    CHECK(forms.key(keyOf(ui::Key::Space))); // Space toggles the focused box off
    CHECK(!shell.valueOf("agree").has_value()); // back to the stored "Off"

    CHECK(forms.press(0, kRedCenter));
    CHECK_EQ(shell.valueOf("color")->text, std::string("Red"));
    const auto state = shell.session().commands().stateId();
    CHECK(forms.press(0, kRedCenter)); // a radio does not toggle off
    CHECK_EQ(shell.session().commands().stateId(), state);
    CHECK(forms.press(0, kBlueCenter));
    CHECK_EQ(shell.valueOf("color")->text, std::string("Blue"));
}

RIVET_TEST(formControllerChoicePopupSelectsWithKeys) {
    Shell shell;
    CHECK(shell.open("choice") != nullptr);
    FormController& forms = *shell.forms;

    CHECK(forms.press(1, kSizeCenter));
    CHECK(forms.popupOpen());
    const auto popup = forms.popup();
    CHECK(popup.has_value());
    CHECK_EQ(popup->labels.size(), std::size_t{3});
    CHECK_EQ(popup->highlighted, std::size_t{1}); // the stored selection
    CHECK(forms.key(keyOf(ui::Key::Down)));
    CHECK_EQ(forms.popup()->highlighted, std::size_t{2});
    CHECK(forms.key(keyOf(ui::Key::Enter)));
    CHECK(!forms.popupOpen());
    CHECK(shell.valueOf("size").has_value());
    CHECK_EQ(shell.valueOf("size")->selected, std::vector<std::uint32_t>{2});

    // Esc closes a reopened popup without a change.
    CHECK(forms.key(keyOf(ui::Key::Space)));
    CHECK(forms.popupOpen());
    CHECK(forms.handleEscape());
    CHECK(!forms.popupOpen());
    CHECK_EQ(shell.valueOf("size")->selected, std::vector<std::uint32_t>{2});
}

RIVET_TEST(formControllerTabTraversesFillableWidgetsInOrder) {
    Shell shell;
    CHECK(shell.open("tab") != nullptr);
    FormController& forms = *shell.forms;

    // Page 0 in /Annots order (the read-only "id" at /Annots 0 is skipped).
    CHECK(forms.key(keyOf(ui::Key::Tab)));
    CHECK_EQ(forms.focus()->field, std::size_t{0}); // name: opens its editor
    CHECK(forms.editorOpen());
    forms.lineEditor().setText("typed");
    CHECK(forms.key(keyOf(ui::Key::Tab))); // commits, moves on
    CHECK_EQ(shell.valueOf("name")->text, std::string("typed"));
    CHECK(!forms.editorOpen());
    CHECK_EQ(forms.focus()->field, std::size_t{1}); // agree
    CHECK(forms.key(keyOf(ui::Key::Tab)));
    CHECK_EQ(forms.focus()->field, std::size_t{2}); // Red
    CHECK_EQ(forms.focus()->widget, std::size_t{0});
    CHECK(forms.key(keyOf(ui::Key::Tab)));
    CHECK_EQ(forms.focus()->widget, std::size_t{1}); // Blue
    CHECK(forms.key(keyOf(ui::Key::Tab)));
    CHECK_EQ(forms.focus()->field, std::size_t{3}); // size (page 1)
    CHECK(forms.key(keyOf(ui::Key::Tab)));
    CHECK_EQ(forms.focus()->field, std::size_t{5}); // notes
    CHECK(forms.editorOpen());
    CHECK(forms.handleEscape()); // cancels the editor ...
    CHECK(forms.key(keyOf(ui::Key::Tab, true))); // ... Shift+Tab goes back
    CHECK_EQ(forms.focus()->field, std::size_t{3});
    CHECK(forms.handleEscape()); // drops the focus
    CHECK(!forms.focus().has_value());
    CHECK(!forms.handleEscape());
}

RIVET_TEST(formControllerReportsReadOnlyAndIgnoresOtherTools) {
    Shell shell;
    CHECK(shell.open("readonly") != nullptr);
    FormController& forms = *shell.forms;

    CHECK(!forms.press(0, kIdCenter)); // passes through (text selection)
    CHECK(shell.lastStatus().find("read-only") != std::string::npos);
    CHECK(!forms.editorOpen());
    CHECK(!shell.valueOf("id").has_value());

    shell.annotations->setTool(AnnotationTool::Rectangle);
    CHECK(!forms.interactive());
    shell.annotations->setTool(AnnotationTool::Select);
    CHECK(forms.interactive());
}

RIVET_TEST(formControllerXfaFormsAreViewOnly) {
    pdf::PdfFormInfoPtr info = sampleForm();
    auto xfa = std::make_shared<pdf::PdfFormInfo>(*info);
    xfa->hasXfa = true;
    Shell shell(xfa);
    CHECK(shell.open("xfa") != nullptr);
    CHECK(shell.statusContains("XFA"));
    CHECK(!shell.forms->press(0, kNameCenter));
    CHECK(!shell.forms->editorOpen());
    CHECK(!shell.forms->key(keyOf(ui::Key::Tab))); // no fillable stop
}

RIVET_TEST(formControllerResetFormAndHighlightCommands) {
    Shell shell;
    CHECK(shell.open("reset") != nullptr);
    FormController& forms = *shell.forms;
    CHECK(!forms.canPerform(FormCommand::ResetForm)); // nothing edited

    CHECK(forms.press(0, kAgreeCenter));
    CHECK(forms.press(0, kNameCenter));
    forms.lineEditor().setText("new");
    CHECK(forms.commitEditor());
    CHECK_EQ(shell.values()->size(), std::size_t{2});
    CHECK(forms.canPerform(FormCommand::ResetForm));

    forms.perform(FormCommand::ResetForm); // one step
    CHECK(shell.values() == nullptr || shell.values()->empty());
    CHECK(shell.session().undo());
    CHECK_EQ(shell.values()->size(), std::size_t{2});

    CHECK(forms.canPerform(FormCommand::HighlightFields));
    CHECK(forms.isChecked(FormCommand::HighlightFields).value_or(true) == false);
    forms.perform(FormCommand::HighlightFields);
    CHECK(forms.isChecked(FormCommand::HighlightFields).value_or(false));
    CHECK(!forms.isChecked(FormCommand::ResetForm).has_value());
}

RIVET_TEST(formControllerWithoutFormsIsInert) {
    Shell shell(nullptr); // backend reports NotAvailable (the PDFium-OFF case)
    CHECK(shell.open("none") != nullptr);
    FormController& forms = *shell.forms;
    CHECK(!forms.press(0, kNameCenter));
    CHECK(!forms.key(keyOf(ui::Key::Tab)));
    CHECK(!forms.canPerform(FormCommand::ResetForm));
    CHECK(!forms.handleEscape());
}

RIVET_TEST(formControllerPaintsEditedValuesHighlightAndPopup) {
    Shell shell;
    CHECK(shell.open("paint") != nullptr);
    FormController& forms = *shell.forms;
    CHECK(forms.press(0, kNameCenter));
    forms.lineEditor().setText("Ada");
    CHECK(forms.commitEditor());
    CHECK(forms.press(0, kAgreeCenter));
    forms.perform(FormCommand::HighlightFields);

    const double zoom = shell.viewport->zoomFactor();
    ui::testing::FakePaintContext paint;
    forms.paintPage(*shell.viewport, 0, core::Rect{0.0, 0.0, 612.0 * zoom, 792.0 * zoom}, paint);
    bool text = false;
    for (const auto& record : paint.texts) {
        if (record.text != "Ada") continue;
        text = true;
        // QA bug 4: the value must land inside its clip (pushClip translates
        // the origin, so viewport-space geometry inside the clip went off the
        // widget and the field looked blank).
        const core::Rect& r = record.absoluteRect;
        const core::Rect& clip = record.absoluteClip;
        CHECK(!clip.isEmpty());
        CHECK(r.minX() >= clip.minX() - 0.5 && r.minX() < clip.maxX());
        CHECK(r.maxY() > clip.minY() && r.minY() < clip.maxY());
    }
    CHECK(text);                    // the edited value is drawn by the overlay
    CHECK(paint.lines.size() >= 2); // the check mark
    CHECK(paint.fills.size() >= 4); // the highlight tint on each fillable widget
    CHECK_EQ(paint.clipDepth, 0);

    CHECK(forms.press(1, kSizeCenter));
    CHECK(forms.popupOpen());
    ui::testing::FakePaintContext above;
    forms.paintAbove(*shell.viewport, above);
    std::size_t labels = 0;
    for (const auto& record : above.texts) labels += record.text == "S" || record.text == "M" || record.text == "L";
    CHECK_EQ(labels, std::size_t{3}); // the popup rows (its page is laid out)
}
