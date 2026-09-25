// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/DocumentWorkspace.hpp"
#include "app/TextLabel.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSession.hpp"
#include "pdf/PdfSystem.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/PageThumbnailList.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TabStrip.hpp"
#include "ui/TextField.hpp"
#include "ui/Toolbar.hpp"
#include "ui/ViewerTextBridge.hpp"
#include "render/ZoomState.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace rivet::app {

// The Rivet shell: composition root for the tab strip, toolbar, page
// thumbnail sidebar, viewport and status bar. It owns the workspace (tabs +
// async open pipeline), the shared task scheduler and the PDF engine, and
// routes keyboard input (focused widget first, then application shortcuts,
// then the viewport).
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
    // IViewerTextBridge over the ACTIVE tab's text service + selection +
    // search state. Forwards to a no-op when no Ready tab is active.
    class ShellTextBridge final : public ui::IViewerTextBridge {
    public:
        explicit ShellTextBridge(ShellController& shell) : shell_(shell) {}

        void warmPage(std::size_t pageIndex) override;
        std::optional<std::uint32_t> charIndexAtPoint(std::size_t pageIndex,
                                                      const core::Point& pagePoint) override;
        std::vector<ui::OverlayRect> overlayRects(std::size_t pageIndex) override;
        void selectionDragBegan(std::size_t pageIndex, std::uint32_t charIndex, bool shiftHeld) override;
        void selectionDragMoved(std::size_t pageIndex, std::uint32_t charIndex) override;
        void selectionDragEnded() override;
        void selectionCleared() override;

    private:
        ShellController& shell_;
    };

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
    void layoutShell();
    void refreshTabStrip();
    void bindActiveTab();
    void setFocus(ui::Widget* widget);
    void setStatus(std::string text);
    void setZoomDisplay(double zoom);
    void updatePageIndicator();
    void updateWindowTitle();

    // Shortcut handlers (command/ctrl based). Returns true when consumed.
    bool handleShortcut(const ui::KeyEvent& event);
    void activateAdjacentTab(int delta);

    // Text interaction (implemented over the active Ready tab).
    DocumentTab* readyActiveTab();
    // Selection as UTF-8 text (line breaks preserved; empty when nothing is
    // selected or the text pages are not loaded).
    std::string selectedText() const;
    void copySelection();
    // Search bar lifecycle + UI updates.
    void setSearchVisible(bool visible);
    void updateSearchUi();
    void revealActiveMatch();
    // Overlay rects for one page of the active tab (selection + search).
    std::vector<ui::OverlayRect> overlayRectsForActiveTab(std::size_t pageIndex) const;

    platform::ShellServices services_;
    // Declaration order = reverse destruction order: the widget tree (root_)
    // dies first (viewport unbinds sessions), then the workspace (sessions,
    // whose teardown needs the scheduler), then the engine, then the
    // scheduler. The background open tasks reference the scheduler and
    // engine, both alive until after the workspace is gone.
    core::TaskScheduler scheduler_;
    std::unique_ptr<pdf::PdfEngine> engine_;
    DocumentWorkspace workspace_;

    // Widget tree. Created in buildWidgets(); root_ owns the whole tree.
    std::unique_ptr<ShellRoot> root_;
    ui::TabStrip* tabStrip_ = nullptr;
    ui::Toolbar* toolbar_ = nullptr;
    ui::PageThumbnailList* sidebar_ = nullptr;
    ui::PdfViewport* viewport_ = nullptr;
    ui::Container* statusBar_ = nullptr;
    TextLabel* overlayLabel_ = nullptr; // loading / error state over the viewport

    // Raw pointers into widgets owned by the containers above.
    TextLabel* zoomLabel_ = nullptr;
    TextLabel* statusLabel_ = nullptr;
    TextLabel* pageCountLabel_ = nullptr;
    TextLabel* pageCaptionLabel_ = nullptr;
    ui::TextField* pageField_ = nullptr;

    // The single focused widget (a text field), or null (viewport focus).
    ui::Widget* focusedWidget_ = nullptr;

    // Text interaction bridge over the active tab (owned; installed on the
    // viewport).
    std::unique_ptr<ShellTextBridge> textBridge_;

    // Search bar (composed widgets, hidden until Cmd+F). All raw pointers
    // into the widget tree owned by root_.
    ui::Container* searchBar_ = nullptr;
    ui::TextField* searchField_ = nullptr;
    TextLabel* searchCountLabel_ = nullptr;
    bool searchVisible_ = false;
};

// Factory used by the platform entry point (main).
std::unique_ptr<ShellController> createShell(const platform::ShellServices& services);

} // namespace rivet::app
