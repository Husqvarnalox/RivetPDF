// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/AnnotationController.hpp"
#include "app/ContentBackend.hpp"
#include "app/ContentInteraction.hpp"
#include "app/ContentLayer.hpp"
#include "app/ContentTool.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/ShellContext.hpp"

#include "core/StrongId.hpp"
#include "core/async/AsyncScope.hpp"
#include "core/async/TaskScheduler.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfContent.hpp"
#include "ui/UiTypes.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rivet::ui {
class TextArea;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

// Page content editing over the ACTIVE Ready tab (ADR-0014/0015):
//   - owns the content tool state (SelectObject / AddText), the per-tab
//     selection, the interaction state machine and the ContentLayer;
//   - turns the machine's intents into editor commands (move, resize, wrap
//     width, delete, retype, add text, replace image, bring to front) through
//     the ContentBackend and runs them on the session's command stack; a
//     failure is reported as "Edit: <reason>" in the status bar;
//   - hosts the INLINE TEXT EDITOR: a TextArea placed over the block's
//     axis-aligned display bounds (font scaled by the zoom, opaque
//     background) that commits exactly one command when it closes;
//   - coalesces arrow-key nudges of one object (same key, <= 500 ms apart)
//     into one undo step: the previous nudge command is undone and one
//     combined move is executed instead.
//
// ONE TOOL STATE with the annotation tools: a content tool other than None
// suspends the annotation layer's input (it keeps painting) and returns the
// annotation tool to Select; choosing any annotation tool returns this
// controller to ContentTool::None.
//
// The selection is stored as an ObjectId (or block id) per tab and pruned
// lazily when it no longer resolves (undo of its creation, deleted object,
// closed page). Document text is never logged.
//
// Main thread only. The controller installs its layer on the viewport and
// uninstalls it in the destructor (the shell destroys controllers first).
class ContentController final : public ContentLayerClient {
public:
    enum class FontFamily : std::uint8_t { Sans, Serif, Mono };

    // The style of text to be added (Add Text defaults) / shown in the bar.
    struct TextStyle {
        FontFamily family = FontFamily::Sans;
        bool bold = false;
        double size = 12.0;
        pdf::PdfColor color{0.0F, 0.0F, 0.0F};
    };

    static constexpr double kNudgeCoalesceSeconds = 0.5;
    static constexpr double kDefaultFontSize = 12.0;
    static constexpr double kMinFontSize = 4.0;
    static constexpr double kMaxFontSize = 200.0;
    static constexpr double kDefaultBoxWidth = 300.0; // page points, click-placed text

    using Clock = std::function<double()>;

    // Builds the (hidden) inline editor into `parent` and installs the layer.
    // `annotations` is the other half of the one-tool state.
    ContentController(ShellContext& context, ui::Widget& parent, AnnotationController& annotations,
                      core::TaskScheduler& scheduler, std::unique_ptr<ContentBackend> backend, Clock clock = {});
    ~ContentController() override;

    ContentController(const ContentController&) = delete;
    ContentController& operator=(const ContentController&) = delete;

    // Binds the active tab: commits the inline editor (against the tab it was
    // opened on), ends gestures, resets the tool to None (tool state resets
    // with the tab), prunes closed tabs.
    void bindTab(DocumentTab* tab);

    // Places the inline editor inside `viewportFrame` (parent space).
    void layout(const core::Rect& viewportFrame);

    // --- Tool state ----------------------------------------------------------------
    ContentTool tool() const { return interaction_.tool(); }
    // Ends gestures, closes (commits) the inline editor, clears the
    // selection; a content tool suspends the annotation layer.
    void setTool(ContentTool tool);
    void toggleTool(ContentTool next) { setTool(tool() == next ? ContentTool::None : next); }

    // Fired when anything the bar shows changed (tool, selection, style).
    void setOnStateChanged(std::function<void()> onStateChanged) { onStateChanged_ = std::move(onStateChanged); }

    // --- Selection and operations (no-ops without a Ready active tab) -------------
    std::optional<ContentInteraction::Selected> selected() const { return currentSelection(); }
    void deleteSelected();
    // Opens the inline editor on the selected block (when it can be retyped).
    void editSelectedText();
    // File dialog -> decode on a worker -> replaceImage.
    void replaceSelectedImage();
    void bringSelectedToFront();
    // Esc priority hook for the shell: cancels the editor / a gesture, else
    // clears the selection. False when there was nothing to do.
    bool handleEscape();

    // --- Text style (bar) -------------------------------------------------------------
    // The style the bar shows: the selected block's, else the Add Text default.
    TextStyle displayedStyle() const;
    // With a selected block these restyle it (one undo step); with the inline
    // editor open on a NEW block they restyle the new block; otherwise they
    // update the Add Text default. Font family / bold only apply to blocks
    // Rivet writes (new blocks and edited blocks).
    void setFontFamily(FontFamily family);
    void setBold(bool bold);
    void setFontSize(double size);
    void setTextColor(const pdf::PdfColor& color);
    // Pixel size of the selected image (0,0 otherwise).
    std::pair<std::uint32_t, std::uint32_t> selectedImagePixels() const;
    bool selectionIsText() const;
    bool selectionIsImage() const;
    bool selectionIsRivetBlock() const;

    // --- Inline text editor ---------------------------------------------------------------
    bool editorOpen() const { return editing_.has_value(); }
    // Commits (one command when something changed / is new) and closes. False
    // (editor left open, text intact) when the commit was refused: a running
    // save holds the editing lock, characters the bundled font cannot write,
    // or the command failed.
    bool commitEditor();
    // Closes without a command.
    void cancelEditor();
    // Commit before save / export / print / close (the pendingEdits hook).
    bool commitPendingEdits() { return commitEditor(); }
    ui::TextArea& editorArea() { return *editorArea_; }

    // --- Menus ---------------------------------------------------------------------------
    bool canPerform(ContentCommand command) const;
    void perform(ContentCommand command);

    // --- Test access -------------------------------------------------------------------------
    void setClock(Clock clock) { clock_ = std::move(clock); }
    const ContentBackend& backend() const { return *backend_; }

    // --- ContentLayerClient (public: tests drive intents directly) ------------------------
    ContentInteraction& interaction() override { return interaction_; }
    const ContentInteraction& interaction() const override { return interaction_; }
    bool active() const override { return interaction_.tool() != ContentTool::None; }
    std::optional<ContentInteraction::Selected> currentSelection() const override;
    void pointerPressed() override;
    void applyIntent(const ContentInteraction::Intent& intent, bool shift) override;
    std::optional<EditingOutline> editingOutline() const override;
    void placeEditor(const ui::ViewportToolHost& host) override;

private:
    struct StoredSelection {
        core::PageId page;
        core::ObjectId id;
        bool isBlock = false;
    };

    struct Editing {
        TabId tab;
        core::PageId page;
        std::size_t pageIndex = 0;
        core::ObjectId block; // existing block (null id for a new one)
        bool isNew = false;
        std::string original;
        core::Rect displayRect; // frame in page display space
        double fontSize = 12.0; // page points
        double lineAdvance = 14.0;
        pdf::PdfColor color;
        // Text that will be written with a bundled face: the face (for the
        // coverage check); nullopt for text kept in its own font.
        std::optional<pdf::PdfBundledFont> bundled;
        bool wrapped = false; // the existing block already has a wrap width
        core::Point origin;   // new block: first baseline start (display)
        double requestedWrap = 0.0; // new block: dragged box width (0 = click)
        std::array<core::Point, 4> quad{};
        double originalMaxY = 0.0;
    };

    struct NudgeState {
        bool valid = false;
        TabId tab;
        core::ObjectId id;
        ContentInteraction::KeyInput key = ContentInteraction::KeyInput::Left;
        core::Point total;
        double time = 0.0;
        std::uint64_t stateAfter = 0;
    };

    DocumentTab* activeTab() const { return context_.readyActiveTab(); }
    bool lockedForEditing(const DocumentTab& tab) const;
    // Runs an edit on `tab`'s session; reports failures as "Edit: <reason>".
    std::optional<std::vector<core::ObjectId>> runOn(DocumentTab& tab, core::Result<editor::ContentEdit> edit);
    std::optional<std::vector<core::ObjectId>> run(core::Result<editor::ContentEdit> edit);

    struct Resolved {
        core::PageId page;
        std::size_t pageIndex = 0;
        ContentInteraction::HitInfo info;
    };
    std::optional<Resolved> resolveSelected() const;
    std::optional<ContentInteraction::HitInfo> infoFor(DocumentTab& tab, core::PageId page, core::ObjectId id,
                                                       bool isBlock) const;
    void select(std::optional<ContentInteraction::Selected> selection);
    void selectStored(DocumentTab& tab, core::PageId page, core::ObjectId id, bool isBlock);
    void announceSelection(const ContentInteraction::HitInfo& info);
    void notifyState();
    void repaint();

    void nudge(const ContentInteraction::Intent& intent, bool shift);
    void deleteInfo(const ContentInteraction::Selected& selection);
    void patchSelectedBlock(editor::TextBlockPatch patch);
    enum class StyleField : std::uint8_t { Font, Size, Color };
    void restyle(const std::function<void(TextStyle&)>& mutate, StyleField field);

    void openEditorOnBlock(const ContentInteraction::Selected& selection);
    void openEditorForNew(std::size_t pageIndex, core::Point topLeft, double boxWidth);
    void showEditor();
    void hideEditor();
    void applyEditorMetrics(double zoom);
    void checkOverflow();
    pdf::PdfBundledFont bundledFor(const TextStyle& style) const;
    static pdf::PdfBundledFont bundledFor(FontFamily family, bool bold);

    void onImageDecoded(TabId tab, core::PageId page, core::ObjectId id,
                        core::Result<std::shared_ptr<const pdf::PdfImageData>> image);

    ShellContext& context_;
    AnnotationController& annotations_;
    core::TaskScheduler& scheduler_;
    std::unique_ptr<ContentBackend> backend_;
    Clock clock_;
    ContentInteraction interaction_;
    ContentLayer layer_;
    // Selected object/block per tab; mutable: pruned by const reads.
    mutable std::unordered_map<TabId, StoredSelection> selected_;
    TextStyle newStyle_;
    std::function<void()> onStateChanged_;
    bool switching_ = false; // setTool() is running (ignore the annotation callback)

    ui::TextArea* editorArea_ = nullptr;
    core::Rect viewportFrame_;
    std::optional<Editing> editing_;
    std::size_t pendingEditsToken_ = 0;
    NudgeState nudge_;
    // Overflow check waiting for the edited block to resolve.
    struct OverflowCheck {
        TabId tab;
        core::PageId page;
        core::ObjectId block;
        double originalMaxY = 0.0;
    };
    std::optional<OverflowCheck> overflowCheck_;
    std::vector<TabId> observedTabs_;

    core::AsyncScope scope_;
    std::shared_ptr<std::atomic<bool>> alive_;
};

} // namespace rivet::app
