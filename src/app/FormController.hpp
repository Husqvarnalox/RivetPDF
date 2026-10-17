// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/AnnotationController.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/FormLayer.hpp"
#include "app/FormLayout.hpp"
#include "app/ShellContext.hpp"

#include "core/StrongId.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfForms.hpp"
#include "ui/UiTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace rivet::ui {
class TextArea;
class TextField;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

// AcroForm filling over the ACTIVE Ready PDF tab (ADR-0024):
//   - fields are live in the Select tool (annotation tool Select, no content
//     tool): a press on a fillable widget focuses it - text fields open the
//     inline editor (a TextField, masked for passwords, or a TextArea for
//     multiline fields) sized to the widget, check boxes and radio buttons
//     toggle, combo and list boxes open a choice popup drawn by the layer;
//   - Tab / Shift+Tab traverse the fillable widgets in page, then /Annots
//     order; Space toggles or opens the popup, Enter commits single-line
//     text (Cmd+Enter multiline), Esc cancels the editor / closes the popup /
//     drops the focus;
//   - each committed value change is one FormValuesCommand on the session's
//     stack (an edit back to the stored value removes the field's edit);
//     Reset Form restores every stored value in one step;
//   - edited fields are drawn by the overlay (their widgets are hidden from
//     the raster); "Highlight Form Fields" tints every fillable widget;
//   - read-only fields, signature fields and documents whose form cannot be
//     filled (XFA, over the limits) are reported in the status bar, as is
//     the presence of field scripts (they never run) once per document.
// Field values are untrusted and never logged. Main thread only. The
// controller installs its layer below the annotation layer and uninstalls
// it in the destructor (the shell destroys controllers first).
class FormController final : public FormLayerClient {
public:
    FormController(ShellContext& context, ui::Widget& parent, AnnotationController& annotations);
    ~FormController() override;

    FormController(const FormController&) = delete;
    FormController& operator=(const FormController&) = delete;

    // Binds the active tab: commits the editor (against its own tab), closes
    // the popup, clears the focus, starts loading the tab's form.
    void bindTab(DocumentTab* tab);
    // Places the inline editors inside `viewportFrame` (parent space).
    void layout(const core::Rect& viewportFrame);

    // --- Menus --------------------------------------------------------------
    bool canPerform(FormCommand command) const;
    void perform(FormCommand command);
    std::optional<bool> isChecked(FormCommand command) const;
    bool highlightFields() const { return highlight_; }
    // Paints the edited values of page `page` (layout index) without the
    // interactive decorations (highlight, focus, hover): the thumbnail
    // overlay (the raster hides edited widgets, QA bug 4).
    void paintThumbnail(std::size_t page, const core::Rect& pageRect, double zoom, ui::PaintContext& context) const;

    // Esc priority hook for the shell: cancels the editor, closes the popup
    // or drops the focus. False when there was nothing to do.
    bool handleEscape();
    // Commits an open editor (the pendingEdits hook). False when refused.
    bool commitEditor();
    void cancelEditor();

    // --- State (tests, shell) -----------------------------------------------
    struct Focus {
        TabId tab;
        core::PageId page;
        std::size_t field = 0;
        std::size_t widget = 0;
    };
    std::optional<Focus> focus() const { return focus_; }
    bool editorOpen() const { return editing_.has_value(); }
    ui::TextField& lineEditor() { return *lineEditor_; }
    ui::TextArea& areaEditor() { return *areaEditor_; }
    // The open popup: its options (labels) and highlighted row.
    struct PopupState {
        std::size_t field = 0;
        std::vector<std::string> labels;
        std::vector<bool> selected;
        std::size_t highlighted = 0;
        bool multi = false;
    };
    std::optional<PopupState> popup() const;
    // Viewport-local frame of the open popup (nullopt when closed or its
    // page is not laid out).
    std::optional<core::Rect> popupFrame(const ui::ViewportToolHost& host) const;
    static constexpr double kPopupRowHeight = 20.0;
    static constexpr std::size_t kPopupMaxRows = 12;
    static constexpr double kPopupMinWidth = 140.0;

    // --- FormLayerClient --------------------------------------------------------
    bool interactive() const override;
    bool popupOpen() const override { return popup_.has_value(); }
    bool popupPress(const ui::ViewportToolHost& host, core::Point viewportPoint) override;
    bool press(std::size_t page, core::Point display) override;
    void pressOutside() override;
    void hover(bool onPage, std::size_t page, core::Point display) override;
    bool key(const ui::KeyEvent& event) override;
    void paintPage(const ui::ViewportToolHost& host, std::size_t page, const core::Rect& pageRect,
                   ui::PaintContext& context) const override;
    void paintAbove(const ui::ViewportToolHost& host, ui::PaintContext& context) const override;
    void placeEditor(const ui::ViewportToolHost& host) override;

private:
    struct Target {
        std::size_t page = 0; // layout index
        core::PageId pageId;
        std::size_t field = 0;
        std::size_t widget = 0;
        core::Rect displayRect;
    };
    struct Editing {
        TabId tab;
        Target target;
        bool multiline = false;
        std::string original;
        double fontSize = 12.0;
    };
    struct Popup {
        TabId tab;
        Target target;
        std::size_t highlighted = 0;
        std::size_t scroll = 0; // first visible row
    };

    DocumentTab* activeTab() const { return context_.readyPdfTab(); }
    pdf::PdfFormInfoPtr formOf(const DocumentTab& tab) const;
    // The widget under `display` on layout page `page` (topmost first).
    std::optional<Target> widgetAt(std::size_t page, core::Point display) const;
    // Re-resolves a focus/editing target against the current model (the
    // page may have moved or vanished).
    std::optional<Target> resolve(const Focus& focus) const;
    std::vector<FormStop> stops() const;

    void focusOn(const Target& target);
    void clearFocus();
    // Acts on the focused field: open the editor, toggle or open the popup.
    void activate(const Target& target);
    void toggle(const Target& target);
    void openEditor(const Target& target);
    void hideEditor();
    void openPopup(const Target& target);
    void closePopup();
    void choose(std::size_t option);
    void traverse(bool backwards);
    bool setValue(const Target& target, pdf::PdfFormValue value);
    void announce(DocumentTab& tab);
    void reportUnfillable(const pdf::PdfFormInfo& info, const pdf::PdfFormField& field);
    void repaint();
    void paintWidgets(std::size_t page, const core::Rect& pageRect, double zoom, ui::PaintContext& context,
                      bool decorations) const;

    ShellContext& context_;
    AnnotationController& annotations_;
    FormLayer layer_;
    ui::TextField* lineEditor_ = nullptr;
    ui::TextArea* areaEditor_ = nullptr;
    core::Rect viewportFrame_;
    bool highlight_ = false;
    std::optional<Focus> focus_;
    std::optional<Editing> editing_;
    std::optional<Popup> popup_;
    std::optional<std::pair<std::size_t, core::Point>> hover_;
    std::size_t pendingEditsToken_ = 0;
    std::vector<TabId> observedTabs_;
    std::unordered_set<TabId> announced_;
};

} // namespace rivet::app
