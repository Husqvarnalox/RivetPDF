// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/CropTool.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/ShellContext.hpp"

#include "core/StrongId.hpp"
#include "editor/PageModel.hpp"
#include "editor/PageSelection.hpp"
#include "ui/PageThumbnailList.hpp"
#include "ui/UiTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rivet::app {

class SidebarController;
class StatusBarController;

// Page-editing commands the shell exposes to shortcuts and platform menus
// (the platform passes them as integers: keep the values stable).
enum class PageEditCommand : std::uint8_t {
    Undo = 0,
    Redo = 1,
    RotateRight = 2,
    RotateLeft = 3,
    DeletePages = 4,
    DuplicatePages = 5,
    MovePagesUp = 6,
    MovePagesDown = 7,
    SelectAllPages = 8,
    Crop = 9,
    ResetCrop = 10,
};

// Application shortcut for a key event, if any: Cmd/Ctrl+Z undo,
// Cmd/Ctrl+Shift+Z redo, Cmd/Ctrl+R rotate right, Cmd/Ctrl+L rotate left.
// (Select-all pages and Delete are thumbnail-focus keys, handled by the
// thumbnail list, so Cmd+A/Delete keep their text meaning elsewhere.)
std::optional<PageEditCommand> pageEditCommandForShortcut(const ui::KeyEvent& event);

// Page editing over the ACTIVE Ready tab:
//   - owns the per-tab editor::PageSelection (keyed by TabId, pruned when
//     tabs close) and mirrors it into the thumbnail list;
//   - wires the thumbnail intents (click gestures, keyboard navigation,
//     delete, select-all, drag-and-drop) to the selection and commands;
//   - runs page commands (rotate, delete, duplicate, move, crop) through
//     the session's command stack, plus undo/redo; failures are reported in
//     the status bar (Command::failure / CommandStack::lastError);
//   - reacts to every page-model change of a bound session (including
//     undo/redo and changes made by other controllers): selection policy,
//     text-selection invalidation, search restart, and - for the active tab
//     - thumbnail relayout, viewport relayout anchored on the tracked page's
//     PageId, and the status bar;
//   - owns the crop tool and installs it on the viewport while cropping.
//
// Commands operate on the page selection, or on the current page when the
// selection is empty.
//
// Main thread only. The controller registers itself as each bound session's
// page-model observer and unregisters in its destructor (the shell destroys
// controllers before the workspace).
class PageEditingController {
public:
    PageEditingController(ShellContext& context, SidebarController& sidebar, StatusBarController& statusBar);
    ~PageEditingController();

    PageEditingController(const PageEditingController&) = delete;
    PageEditingController& operator=(const PageEditingController&) = delete;

    // Binds the active tab (null / not Ready = nothing to edit). Ends the
    // crop tool, installs the session observer, pushes the selection.
    void bindTab(DocumentTab* tab);

    bool canPerform(PageEditCommand command) const;
    // Runs the command when possible (a status message otherwise).
    void perform(PageEditCommand command);

    // Shortcut routing: true when the event mapped to a command (which ran
    // or reported why not).
    bool handleShortcut(const ui::KeyEvent& event);
    // Keys for the active viewport tool (Enter/Esc while cropping). True
    // when consumed.
    bool handleToolKey(const ui::KeyEvent& event);

    // --- Operations (all no-ops without a Ready active tab) ---------------
    void rotate(int degrees);
    void deletePages();
    void duplicatePages();
    // Moves the target pages as one block by one position (up = -1).
    void movePagesBy(int delta);
    // Moves the target pages as one block so the first lands at `index`
    // (clamped to the valid range).
    void movePagesTo(std::size_t index);
    // Drop at a GAP of the current order (thumbnail drag and drop).
    void dropSelectionAtGap(std::size_t gap);
    void selectAllPages();
    void undo();
    void redo();
    void beginCrop();
    void resetCrop();

    // Thumbnail intents (the list's callbacks; public for tests).
    void handleRowClicked(std::size_t row, ui::PageThumbnailList::ClickGesture gesture);
    void handleNavigate(int delta, bool extend);

    // The active tab's page selection (null without a Ready active tab).
    editor::PageSelection* activeSelection();
    // Pages a command applies to: the selection in model order, or the
    // current page.
    std::vector<core::PageId> targetPages() const;

    CropTool& cropTool() { return cropTool_; }
    bool isCropping() const { return cropTool_.isActive(); }

    // "N pages selected" / "" (what the status bar shows).
    std::string selectionSummary() const;

private:
    DocumentTab* activeTab() const;
    // The tab's stored selection, or an empty one (const reads).
    const editor::PageSelection& selectionOf(const DocumentTab* tab) const;
    // Executes on the active tab's session; reports a failure as
    // "<what>: <reason>". Returns success.
    bool run(std::unique_ptr<editor::Command> command, const char* what);
    void handlePageModelChanged(TabId tabId, const editor::PageModelChange& change);
    void syncSelectionView();
    void endCrop();
    void moveBlock(const std::vector<core::PageId>& ids, std::size_t destination);
    void prunePageSelections();

    ShellContext& context_;
    SidebarController& sidebar_;
    StatusBarController& statusBar_;
    ui::PageThumbnailList& thumbnails_;
    std::unordered_map<TabId, editor::PageSelection> selections_;
    // Sessions whose page-model observer points at this controller.
    std::vector<TabId> observedTabs_;
    CropTool cropTool_;
    // Pages the running crop applies to (fixed when the tool starts).
    std::vector<core::PageId> cropTargets_;
};

} // namespace rivet::app
