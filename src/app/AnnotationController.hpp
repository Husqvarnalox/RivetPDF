// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/AnnotationInteraction.hpp"
#include "app/AnnotationLayer.hpp"
#include "app/AnnotationTools.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/ShellContext.hpp"

#include "core/StrongId.hpp"
#include "core/geometry/Rect.hpp"
#include "editor/AnnotationCommands.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace rivet::ui {
class Button;
class Container;
class TextArea;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

// Annotation commands the shell exposes to platform menus (integers on the
// platform side: keep the values stable). 0..10 equal the AnnotationTool
// values.
enum class AnnotationCommand : std::uint8_t {
    ToolSelect = 0,
    ToolHighlight = 1,
    ToolUnderline = 2,
    ToolStrikeOut = 3,
    ToolNote = 4,
    ToolInk = 5,
    ToolRectangle = 6,
    ToolEllipse = 7,
    ToolLine = 8,
    ToolArrow = 9,
    ToolStamp = 10,
    DeleteAnnotation = 11,
    EditNote = 12,
};

// Annotation editing over the ACTIVE Ready tab (ADR-0013):
//   - owns the current tool, the per-tool remembered styles, the per-tab
//     selected annotation and the interaction state machine;
//   - is the ViewportLayer's client: turns the machine's intents into editor
//     commands (create, move, resize, restyle, delete, edit contents) and
//     runs them on the session's command stack; a failure is reported as
//     "Annotate: <reason>" in the status bar and nothing is changed;
//   - converts the text selection to highlight/underline/strike-out markup
//     in ONE command (one undo step) - all-or-nothing across pages;
//   - hosts the note editor: a small panel (TextArea + Done) that commits
//     exactly one editContents command when it closes.
//
// The selection is stored as an AnnotationId per tab and pruned lazily when
// it no longer resolves (undo of its creation, deletion, closed page).
// Annotation contents/author are never logged.
//
// Main thread only. The controller installs its layer on the viewport and
// uninstalls it in the destructor (the shell destroys controllers first).
class AnnotationController final : public AnnotationLayerClient {
public:
    static constexpr double kNotePanelWidth = 236.0;
    static constexpr double kNotePanelHeight = 148.0;

    // Builds the (hidden) note editor into `parent` and installs the layer.
    AnnotationController(ShellContext& context, ui::Widget& parent);
    ~AnnotationController() override;

    AnnotationController(const AnnotationController&) = delete;
    AnnotationController& operator=(const AnnotationController&) = delete;

    // Binds the active tab: ends any gesture, commits and closes the note
    // editor (against the tab it was opened on), prunes closed tabs.
    void bindTab(DocumentTab* tab);

    // Places the note editor inside `viewportFrame` (parent space).
    void layout(const core::Rect& viewportFrame);

    // --- Tool and style state ----------------------------------------------
    AnnotationTool tool() const { return interaction_.tool(); }
    // Closes the note editor, ends gestures, deselects the annotation and -
    // for a markup tool with a text selection - converts it immediately.
    void setTool(AnnotationTool tool);
    const ToolStyle& toolStyle(AnnotationTool tool) const { return styles_[static_cast<std::size_t>(tool)]; }
    // The style the toolbar shows: the selected annotation's, else the
    // current tool's default.
    editor::AnnotationStyle displayedStyle() const;
    // The tool whose style controls the toolbar addresses: the selected
    // annotation's kind, else the current tool (Select when neither applies).
    AnnotationTool styleTool() const;
    pdf::PdfStampName displayedStampName() const;
    // With a selection these restyle it (one undo step); otherwise they
    // update the current tool's default. The stamp name only ever updates the
    // default (a stamp's name is not editable after creation).
    void setColor(const pdf::PdfColor& color);
    void setOpacity(float opacity);
    void setBorderWidth(float width);
    void setFillEnabled(bool enabled);
    void setStampName(pdf::PdfStampName name);

    // Fired when anything the toolbar shows changed (tool, style, selection).
    void setOnStateChanged(std::function<void()> onStateChanged) { onStateChanged_ = std::move(onStateChanged); }

    // --- Selection and operations (all no-ops without a Ready active tab) ---
    std::optional<core::AnnotationId> selectedId() const;
    void deleteSelected();
    void editSelectedNote();
    // Markup from the active tab's text selection with a markup tool.
    void convertTextSelection(AnnotationTool tool);

    // --- Note editor ---------------------------------------------------------
    bool noteEditorOpen() const { return editing_.has_value(); }
    // Commits (one editContents command when the text changed) and closes.
    void closeNoteEditor();
    // Commits an open note editor's text as an undoable edit before the
    // document is saved, exported, printed or closed. False (editor left
    // open, text intact) when a running save holds the editing lock.
    bool commitPendingEdits();
    ui::TextArea& noteArea() { return *noteArea_; }
    ui::Button& noteDoneButton() { return *noteDone_; }

    // --- Menus -----------------------------------------------------------------
    bool canPerform(AnnotationCommand command) const;
    void perform(AnnotationCommand command);

    // --- AnnotationLayerClient (public: tests drive intents directly) -----------
    AnnotationInteraction& interaction() override { return interaction_; }
    const AnnotationInteraction& interaction() const override { return interaction_; }
    std::optional<AnnotationInteraction::Selected> currentSelection() const override;
    std::shared_ptr<const std::vector<editor::AnnotationView>>
    pageAnnotations(std::size_t pageIndex) const override;
    editor::AnnotationStyle previewStyle() const override;
    bool textSelectionNonEmpty() const override;
    void pointerPressed() override { closeNoteEditor(); }
    void applyIntent(const AnnotationInteraction::Intent& intent) override;

private:
    struct Editing {
        TabId tab;
        core::AnnotationId id;
        std::string original;
    };

    DocumentTab* activeTab() const { return context_.readyActiveTab(); }
    // Runs an edit on the ACTIVE tab's session; reports failures as
    // "Annotate: <reason>". Returns the affected ids on success.
    std::optional<std::vector<core::AnnotationId>> run(core::Result<editor::AnnotationEdit> edit);
    bool lockedForEditing(const DocumentTab& tab) const;
    // The selected annotation's view and page, pruning a stale selection.
    struct Resolved {
        core::PageId page;
        std::size_t pageIndex = 0;
        editor::AnnotationView view;
    };
    std::optional<Resolved> resolveSelected() const;
    void select(std::optional<core::AnnotationId> id);
    void notifyState();
    void repaint();
    void create(const AnnotationInteraction::Intent& intent);
    void restyleSelection(editor::StylePatch patch);
    editor::AnnotationDraft draftFor(AnnotationTool tool, core::PageId page) const;
    void openNoteEditor(core::AnnotationId id);
    void placeNotePanel();
    void hideNotePanel();

    ShellContext& context_;
    AnnotationInteraction interaction_;
    AnnotationLayer layer_;
    std::array<ToolStyle, kAnnotationToolCount> styles_;
    // Selected annotation per tab; mutable: pruned by const reads.
    mutable std::unordered_map<TabId, core::AnnotationId> selected_;
    std::function<void()> onStateChanged_;

    // Raw pointers into widgets owned by the parent's tree.
    ui::Container* notePanel_ = nullptr;
    ui::TextArea* noteArea_ = nullptr;
    ui::Button* noteDone_ = nullptr;
    core::Rect viewportFrame_;
    std::optional<Editing> editing_;
    std::size_t pendingEditsToken_ = 0;
    // Tabs whose session carries our originals-loaded observer.
    std::vector<TabId> observedTabs_;
};

} // namespace rivet::app
