// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/DocumentWorkspace.hpp"
#include "app/PasswordPromptController.hpp"
#include "app/SearchBarController.hpp"
#include "app/ShellContext.hpp"
#include "app/SidebarController.hpp"
#include "app/StatusBarController.hpp"
#include "app/TextInteractionController.hpp"
#include "app/TextLabel.hpp"
#include "core/async/TaskScheduler.hpp"
#include "pdf/PdfSystem.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TabStrip.hpp"
#include "ui/Toolbar.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace rivet::app {

// The Rivet shell: composition root for the tab strip, toolbar, sidebar,
// viewport, find bar, password prompt and status bar. It owns the workspace
// (tabs + async open pipeline), the shared task scheduler and the PDF engine,
// builds the widget tree, lays it out, owns keyboard focus and routes key
// input (focused widget first, then Escape priorities and application
// shortcuts, then the viewport).
//
// Feature logic lives in focused controllers that share a ShellContext:
// TextInteractionController (selection, copy, links), SearchBarController,
// SidebarController (thumbnails + outline), PasswordPromptController and
// StatusBarController. The shell binds them to the active tab and handles
// tabs, window title, presentation mode, open and print itself.
//
// The shell deliberately owns NO document/view state: zoom, scroll and the
// current page live in the active tab's ViewerState (bound by the viewport),
// sessions live in the workspace. This shell is wiring only.
//
// Main-thread only.
class ShellController {
public:
    // Builds the shell. The services are owned by the platform host and must
    // outlive the controller.
    static std::unique_ptr<ShellController> create(const platform::ShellServices& services);
    ~ShellController();

    ShellController(const ShellController&) = delete;
    ShellController& operator=(const ShellController&) = delete;

    // Root of the widget tree; the host sets its frame to the window content
    // bounds (which triggers the shell layout) and forwards input/paint to it.
    ui::Widget& rootWidget() { return *root_; }

    // Keyboard entry point: the host forwards key events here. Priority:
    // focused widget (text fields) -> application shortcuts -> viewport.
    bool handleKeyEvent(const ui::KeyEvent& event);

    // Triggered by the Open button / menu; shows the platform dialog and
    // opens the chosen document (asynchronously in a new tab).
    void handleOpenRequest();

    // Opens a document asynchronously in a new (or already open) tab. Used
    // by handleOpenRequest() and the platform entry point for
    // open-on-launch (`rivet file.pdf`).
    void openDocument(const std::filesystem::path& path);

private:
    // Root container that re-runs the shell layout whenever its frame changes.
    class ShellRoot final : public ui::Container {
    public:
        std::function<void()> onLayout;
        void layout() override {
            if (onLayout) onLayout();
        }
    };

    explicit ShellController(const platform::ShellServices& services);

    void buildWidgets();
    void buildToolbar();
    void layoutShell();
    void refreshTabStrip();
    void bindActiveTab();
    void setFocus(ui::Widget* widget);
    void setStatus(std::string text);
    void setZoomDisplay(double zoom);
    void updateWindowTitle();

    // Shortcut handlers (command/ctrl based). Returns true when consumed.
    bool handleShortcut(const ui::KeyEvent& event);
    void activateAdjacentTab(int delta);

    // The active tab when it is Ready (session attached), otherwise null.
    DocumentTab* readyActiveTab() { return context_->readyActiveTab(); }

    // Presentation mode: chrome hidden, one page centered; Esc exits.
    void setPresentationMode(bool enabled);
    bool presentationMode() const { return presentationMode_; }
    void handlePrintRequest();

    platform::ShellServices services_;
    // Declaration order = reverse destruction order: the feature controllers
    // die first (they only hold pointers into the tree and the workspace;
    // nothing calls into them during teardown), then the context, then the
    // widget tree (root_; the destructor already unbound the viewport), then
    // the workspace (sessions, whose teardown needs the scheduler), then the
    // engine, then the scheduler. The background open tasks reference the
    // scheduler and engine, both alive until after the workspace is gone.
    core::TaskScheduler scheduler_;
    std::unique_ptr<pdf::PdfEngine> engine_;
    DocumentWorkspace workspace_;

    // Widget tree. Created in buildWidgets(); root_ owns the whole tree.
    std::unique_ptr<ShellRoot> root_;
    ui::TabStrip* tabStrip_ = nullptr;
    ui::Toolbar* toolbar_ = nullptr;
    ui::PdfViewport* viewport_ = nullptr;
    TextLabel* overlayLabel_ = nullptr; // loading / error state over the viewport
    TextLabel* zoomLabel_ = nullptr;

    // The single focused widget (a text field), or null (viewport focus).
    ui::Widget* focusedWidget_ = nullptr;
    bool presentationMode_ = false;

    // Shared by the controllers below (references members above).
    std::unique_ptr<ShellContext> context_;
    std::unique_ptr<TextInteractionController> textInteraction_; // installed on the viewport
    std::unique_ptr<SidebarController> sidebar_;
    std::unique_ptr<SearchBarController> searchBar_;
    std::unique_ptr<PasswordPromptController> passwordPrompt_;
    std::unique_ptr<StatusBarController> statusBar_;
};

// Factory used by the platform entry point (main).
std::unique_ptr<ShellController> createShell(const platform::ShellServices& services);

} // namespace rivet::app
