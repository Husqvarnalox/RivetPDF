// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/AnnotationBarController.hpp"
#include "app/AnnotationController.hpp"
#include "app/ComparisonHostView.hpp"
#include "app/ContentBarController.hpp"
#include "app/ContentController.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/FileController.hpp"
#include "app/FormController.hpp"
#include "app/OcrController.hpp"
#include "app/RedactionController.hpp"
#include "app/MarkdownExportController.hpp"
#include "app/MarkdownHostView.hpp"
#include "app/MenuModel.hpp"
#include "app/PageEditingController.hpp"
#include "app/PasswordPromptController.hpp"
#include "app/PrintCoordinator.hpp"
#include "app/RecentFiles.hpp"
#include "app/SearchBarController.hpp"
#include "app/ShellContext.hpp"
#include "app/SidebarController.hpp"
#include "app/SignatureSheetController.hpp"
#include "app/StatusBarController.hpp"
#include "app/TextInteractionController.hpp"
#include "app/TextLabel.hpp"
#include "core/async/TaskScheduler.hpp"
#include "pdf/PdfSystem.hpp"
#include "platform/PlatformKit.hpp"
#include "core/async/AsyncScope.hpp"
#include "ui/Animation.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/PopupMenu.hpp"
#include "ui/TabStrip.hpp"
#include "ui/Toolbar.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rivet::ui {
class Button;
class ChromeBar;
class IconButton;
class SegmentedControl;
class Tooltip;
class ZoomControl;
} // namespace rivet::ui

namespace rivet::app {

class WelcomeView;

// The shell's tool modes (ADR-0033 §5): the toolbar's segmented control.
enum class ToolMode : std::uint8_t { View, Annotate, Edit, Forms, Redact };

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
    // Same with a caller-supplied PDF engine (tests inject a fake one).
    static std::unique_ptr<ShellController> createWithEngine(const platform::ShellServices& services,
                                                             std::unique_ptr<pdf::PdfEngine> engine);
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

    // Page editing for platform menus: runs the command on the active tab
    // (a status message otherwise). canPerformPageEdit drives menu item
    // validation (enabled/disabled). Undo/Redo are kind-aware: on a Markdown
    // tab they run that tab's text command stack; the page commands are
    // PDF-only.
    void performPageEdit(PageEditCommand command);
    bool canPerformPageEdit(PageEditCommand command) const;

    // Annotation tools for platform menus (same validation pattern). A tool
    // command also reveals the annotation strip.
    void performAnnotation(AnnotationCommand command);
    bool canPerformAnnotation(AnnotationCommand command) const;

    // Content editing (Edit / Add Text tools, selection commands) for platform
    // menus; same validation pattern. A tool command also reveals the
    // properties bar (it shows while a content tool is active).
    void performContent(ContentCommand command);
    bool canPerformContent(ContentCommand command) const;

    // File lifecycle for platform menus (same validation pattern).
    void performFile(FileCommand command);
    bool canPerformFile(FileCommand command) const;

    // Markdown display mode of the active Markdown tab (Cmd+1/2/3, toolbar
    // buttons, View menu). Inert for other tabs.
    void performMarkdownMode(MarkdownDisplayMode mode);
    bool canPerformMarkdownMode() const;
    // View-menu zoom commands: PDF viewport, or both comparison viewports.
    void performView(ViewCommand command);
    // Copy / Select All / Find / Find Next / Previous / Print (menu bar).
    void performShell(ShellCommand command);
    bool canPerformShell(ShellCommand command) const;
    bool canPerformView() const;
    // The mode of the active Ready Markdown tab, if any (menu check marks).
    std::optional<MarkdownDisplayMode> activeMarkdownMode() const;

    // Portable menu dispatch (see MenuModel.hpp): routes any AppCommand to the
    // perform*/canPerform* methods above. isChecked is the check/radio state
    // (Markdown display mode); nullopt for commands without one. Quit calls
    // the handler set by setQuitHandler (macOS quits via its app menu and
    // never sets one; canPerform(Quit) is false without a handler).
    void perform(const AppCommand& command);
    bool canPerform(const AppCommand& command) const;
    std::optional<bool> isChecked(const AppCommand& command) const;
    void setQuitHandler(std::function<void()> handler) { quitHandler_ = std::move(handler); }

    // Read access for the platform host and tests.
    DocumentWorkspace& workspace() { return workspace_; }
    ui::PdfViewport& pdfViewport() { return *viewport_; }
    MarkdownHostView& markdownHost() { return *markdownView_; }
    ComparisonHostView& comparisonHost() { return *comparisonView_; }
    bool comparisonChromeActive() const { return chromeComparison_; }
    ui::Toolbar& toolbar() { return *toolbar_; }
    ui::TabStrip& tabStrip() { return *tabStrip_; }
    SearchBarController& searchBar() { return *searchBar_; }
    // Toolbar items that only make sense with a document open (tests).
    const std::vector<ui::Widget*>& pdfToolbarItems() const { return pdfToolbarItems_; }
    const std::vector<ui::Widget*>& documentToolbarItems() const { return documentToolbarItems_; }
    // Tool mode (View | Annotate | Edit | Forms | Redact), derived from the
    // controllers' tool state; setToolMode switches exclusively.
    ToolMode toolMode() const;
    void setToolMode(ToolMode mode);
    WelcomeView& welcomeView() { return *welcome_; }
    RecentFiles& recentFiles() { return recentFiles_; }
    bool sidebarVisible() const { return sidebarVisible_; }
    void setSidebarVisible(bool visible);
    double sidebarWidth() const { return sidebarWidth_; }
    void setSidebarWidth(double width);
    ui::PopupMenu& popupMenu() { return *popup_; }
    // Opens the toolbar's overflow menu (every menu-bar command; tests).
    void openOverflowMenu();
    void openZoomMenu(const core::Rect& anchor);
    FileController& fileController() { return *fileLifecycle_; }
    FormController& forms() { return *forms_; }
    PasswordPromptController& passwordPrompt() { return *passwordPrompt_; }
    StatusBarController& statusBar() { return *statusBar_; }
    MarkdownExportController& markdownExport() { return *markdownExport_; }

    // Tab close request (tab strip / Cmd+W): runs the dirty-document prompt
    // and closes the tab when permitted (or when its save completes).
    void requestCloseTab(std::size_t index);

    // --- Read-only probes (scripted QA harness, tests) ---------------------
    // True when nothing is in flight: no tab is opening, no save runs, the
    // Markdown parser is settled, the worker pool is drained and the last
    // paint of a visible PDF viewport had every tile. A snapshot; callers
    // poll it (the QA runner requires several idle polls in a row).
    bool isIdle() const;
    // Why the shell is not idle (empty when idle); for diagnostics.
    std::string_view idleBlocker() const;
    // Current status-bar message.
    const std::string& statusText() const;
    // The find bar's result text ("2 of 5", "No results"; empty when hidden).
    std::string searchCountText() const;

private:
    // Root container that re-runs the shell layout whenever its frame changes.
    class ShellRoot final : public ui::Container {
    public:
        std::function<void()> onLayout;
        std::function<void(const ui::PointerEvent&, bool)> onPointer;
        void layout() override {
            if (onLayout) onLayout();
        }
        bool onMouse(const ui::PointerEvent& event) override {
            const bool handled = ui::Container::onMouse(event);
            if (onPointer) onPointer(event, handled);
            return handled;
        }
    };

    // Runs a tooltip delay on the worker pool and delivers it on the main
    // thread; fenced by delayScope_ and alive_ (nothing runs after the
    // shell's destructor started).
    class DelayedRunner final : public ui::IDelayedRunner {
    public:
        explicit DelayedRunner(ShellController& shell) : shell_(shell) {}
        void runAfter(std::chrono::milliseconds delay, std::function<void()> task) override;

    private:
        ShellController& shell_;
    };

    ShellController(const platform::ShellServices& services, std::unique_ptr<pdf::PdfEngine> engine);

    void buildWidgets();
    // Reads the platform appearance (light/dark, accent) into the current
    // ui theme and repaints the tree (ADR-0033). Light/default without one.
    void applyAppearance();
    void buildToolbar();
    void buildContextBars();
    // Enables/disables and checks the chrome controls from canPerform().
    void syncChrome();
    void syncToolMode();
    // Hover tracking for the whole tree (pointer exit, tooltips).
    void trackPointer(const ui::PointerEvent& event, bool handled);
    std::string tooltipFor(const char* title, const AppCommand& command) const;
    std::vector<ui::PopupMenu::Item> overflowItems() const;
    void showPopup(std::vector<ui::PopupMenu::Item> items, ui::Widget& anchorWidget, const core::Rect& localAnchor);
    void refreshWelcome();
    void layoutShell();
    void refreshTabStrip();
    void bindActiveTab();
    void bindMarkdownTab(DocumentTab* tab);
    void bindComparisonTab(DocumentTab* tab);
    // File > Compare With... (ADR-0029): pick a second PDF, open the comparison tab.
    void performCompare();
    bool canPerformCompare() const;
    // Shows the chrome of the active tab's kind (PDF toolbar items + sidebar
    // + bars vs. the Markdown mode switcher) and relayouts when it changed.
    void applyChromeForKind(bool markdown, bool comparison = false);
    bool markdownChromeActive() const { return chromeMarkdown_; }
    void syncModeButtons();
    void onMarkdownChanged(TabId tab);
    void updateDocumentEdited();
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
    // Annotate > Sign... (manage = false) / Manage Signatures... (ADR-0025).
    void openSignatureSheet(bool manage);
    // Cancels (and waits for) a print spool that renders the tab's document;
    // must run before the tab's session is destroyed.
    void cancelPrintForTab(std::size_t index);

    platform::ShellServices services_;
    std::function<void()> quitHandler_;
    // Tooltip delays (see DelayedRunner). alive_ is reset first in the
    // destructor; delayScope_ outlives nothing it fences (declared before
    // the scheduler: destroyed after the posted work finished).
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    // Delay tasks (tooltips, caret blink) currently occupying the worker
    // pool: isIdle() does not count them as pending work.
    std::shared_ptr<std::atomic<std::size_t>> delayedTasks_ = std::make_shared<std::atomic<std::size_t>>(0);
    RecentFiles recentFiles_;
    // Declaration order = reverse destruction order: the feature controllers
    // die first (they only hold pointers into the tree and the workspace;
    // nothing calls into them during teardown), then the context, then the
    // widget tree (root_; the destructor already unbound the viewport), then
    // the workspace (sessions, whose teardown needs the scheduler), then the
    // engine, then the scheduler. The background open tasks reference the
    // scheduler and engine, both alive until after the workspace is gone.
    core::TaskScheduler scheduler_;
    core::AsyncScope delayScope_;
    DelayedRunner delayedRunner_{*this};
    std::unique_ptr<pdf::PdfEngine> engine_;
    DocumentWorkspace workspace_;

    // Widget tree. Created in buildWidgets(); root_ owns the whole tree.
    std::unique_ptr<ShellRoot> root_;
    ui::TabStrip* tabStrip_ = nullptr;
    ui::Toolbar* toolbar_ = nullptr;
    ui::IconButton* sidebarButton_ = nullptr;
    ui::IconButton* saveButton_ = nullptr;
    ui::IconButton* prevPageButton_ = nullptr;
    ui::IconButton* nextPageButton_ = nullptr;
    ui::IconButton* searchButton_ = nullptr;
    ui::IconButton* printButton_ = nullptr;
    ui::ZoomControl* zoomControl_ = nullptr;
    ui::Container* pageNavigator_ = nullptr;
    ui::SegmentedControl* toolModes_ = nullptr;
    ui::SegmentedControl* markdownModes_ = nullptr;
    // Context bars of the Forms / Redact modes and of Markdown tabs.
    ui::ChromeBar* formsBar_ = nullptr;
    ui::ChromeBar* redactBar_ = nullptr;
    ui::ChromeBar* formatBar_ = nullptr;
    ui::Button* highlightFieldsButton_ = nullptr;
    std::vector<std::pair<ui::Widget*, AppCommand>> commandControls_; // enabled from canPerform()
    ui::Widget* sidebarSplitter_ = nullptr;
    WelcomeView* welcome_ = nullptr;
    ui::PopupMenu* popup_ = nullptr;
    ui::Tooltip* tooltip_ = nullptr;
    const ui::Widget* hoveredWidget_ = nullptr;
    bool formsMode_ = false;
    bool sidebarVisible_ = true;
    double sidebarWidth_ = 220.0;
    ui::PdfViewport* viewport_ = nullptr;
    MarkdownHostView* markdownView_ = nullptr; // content slot of Markdown tabs
    ComparisonHostView* comparisonView_ = nullptr; // content slot of comparison tabs
    // Toolbar items per kind (hidden/shown by applyChromeForKind).
    std::vector<ui::Widget*> pdfToolbarItems_;
    std::vector<ui::Widget*> markdownToolbarItems_;
    // Save / Find / Print: hidden (not just disabled) while no document is open.
    std::vector<ui::Widget*> documentToolbarItems_;
    bool chromeMarkdown_ = false;
    bool chromeComparison_ = false;
    TextLabel* overlayLabel_ = nullptr; // loading / error state over the viewport

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
    std::unique_ptr<PageEditingController> pageEditing_;
    std::unique_ptr<FileController> fileLifecycle_;
    // Export as PDF / printing of Markdown tabs (references printCoordinator_;
    // reset first in the destructor).
    std::unique_ptr<MarkdownExportController> markdownExport_;
    std::unique_ptr<AnnotationController> annotations_;
    std::unique_ptr<AnnotationBarController> annotationBar_;
    // Declared after annotations_: destroyed before it (the content tool
    // suspends the annotation layer and uninstalls itself on destruction).
    std::unique_ptr<ContentController> content_;
    std::unique_ptr<ContentBarController> contentBar_;
    // AcroForm filling (ADR-0024). Declared after annotations_: destroyed
    // before it (it reads the annotation tool state and owns a viewport layer).
    std::unique_ptr<FormController> forms_;
    // Redaction (ADR-0026). Declared after content_: destroyed before it (it
    // reads and switches the shared tool state and owns a viewport layer).
    std::unique_ptr<RedactionController> redaction_;
    // Text recognition (ADR-0027); reads the shell context only.
    std::unique_ptr<OcrController> ocr_;
    std::unique_ptr<SignatureSheetController> signatureSheet_;

    // Print flow (panel -> worker spool -> platform print). Declared last:
    // destroyed first, while the sessions its worker borrows are alive.
    PrintCoordinator printCoordinator_;
};

// Factory used by the platform entry point (main).
std::unique_ptr<ShellController> createShell(const platform::ShellServices& services);

} // namespace rivet::app
