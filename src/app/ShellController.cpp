// SPDX-License-Identifier: MPL-2.0
#include "app/ShellController.hpp"

#include "app/MarkdownParseCoordinator.hpp"
#include "app/MarkdownPreviewView.hpp"
#include "app/RedactionController.hpp"

#include "app/WelcomeView.hpp"
#include "core/Error.hpp"
#include "ui/Button.hpp"
#include "ui/IconButton.hpp"
#include "ui/Icons.hpp"
#include "ui/SegmentedControl.hpp"
#include "ui/TextField.hpp"
#include "ui/Theme.hpp"
#include "ui/Tooltip.hpp"
#include "ui/ZoomControl.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <thread>
#include <utility>

namespace rivet::app {

namespace {

// "⌘S" on macOS, "Ctrl+S" elsewhere (tooltips and the overflow menu).
std::string shortcutText(const Shortcut& shortcut) {
    std::string key = shortcut.key;
    for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
#if defined(__APPLE__)
    std::string text;
    if (shortcut.alt) text += "\xE2\x8C\xA5";   // ⌥
    if (shortcut.shift) text += "\xE2\x87\xA7"; // ⇧
    if (shortcut.primary) text += "\xE2\x8C\x98"; // ⌘
    return text + key;
#else
    std::string text;
    if (shortcut.primary) text += "Ctrl+";
    if (shortcut.alt) text += "Alt+";
    if (shortcut.shift) text += "Shift+";
    return text + key;
#endif
}

// The sidebar's resize handle (ADR-0033 §5: 220 px, resizable): a 6 px hit
// area straddling the sidebar's right edge that draws the separator line.
class SidebarSplitter final : public ui::Widget {
public:
    explicit SidebarSplitter(std::function<void(double)> onResize) : onResize_(std::move(onResize)) {}
    bool onMouse(const ui::PointerEvent& event) override {
        const double rootX = event.position.x + originInRoot().x;
        switch (event.type) {
        case ui::PointerEventType::Down:
            dragging_ = true;
            grabOffset_ = event.position.x - 3.0;
            return true;
        case ui::PointerEventType::Move:
            if (!dragging_ || event.button == 0) {
                dragging_ = false;
                return bounds().contains(event.position);
            }
            if (onResize_) onResize_(rootX - grabOffset_);
            return true;
        case ui::PointerEventType::Up:
            dragging_ = false;
            return true;
        default: return false;
        }
    }
    void paintSelf(ui::PaintContext& context) const override {
        context.fillRect(core::Rect{2.0, 0.0, 1.0, frame().size.height}, ui::colors().chromeSeparator);
    }

private:
    std::function<void(double)> onResize_;
    bool dragging_ = false;
    double grabOffset_ = 0.0;
};

} // namespace

std::unique_ptr<ShellController> ShellController::create(const platform::ShellServices& services) {
    return createWithEngine(services, pdf::createEngine());
}

std::unique_ptr<ShellController> ShellController::createWithEngine(const platform::ShellServices& services,
                                                                    std::unique_ptr<pdf::PdfEngine> engine) {
    auto controller = std::unique_ptr<ShellController>(new ShellController(services, std::move(engine)));
    controller->buildWidgets();
    return controller;
}

ShellController::ShellController(const platform::ShellServices& services, std::unique_ptr<pdf::PdfEngine> engine)
    : services_(services),
      recentFiles_(services.appData),
      scheduler_(0),
      engine_(std::move(engine)),
      workspace_(*engine_, scheduler_, services.mainDispatcher),
      printCoordinator_(workspace_, *engine_, scheduler_, services.mainDispatcher, services.printService,
                        [this](std::string text) { setStatus(std::move(text)); }) {}

ShellController::~ShellController() {
    alive_.reset(); // no tooltip delay runs from here on
    delayScope_.closeAndWait();
    if (services_.appearance != nullptr) services_.appearance->onChange(nullptr);
    // The Markdown exporter's workers use the engine and hand documents to
    // the print coordinator: stop them first.
    if (fileLifecycle_ != nullptr) fileLifecycle_->setMarkdownExporter(nullptr);
    markdownExport_.reset();
    // A print spool borrows a session's document: stop it before anything
    // is torn down.
    printCoordinator_.cancel();
    // The viewport must drop its render-source/layout/state pointers before
    // the sessions and the widget tree die.
    viewport_->clearDocument();
    markdownView_->bind(nullptr);
    comparisonView_->bind(nullptr);
    // The controllers die before the widget tree: the viewport must not keep
    // a pointer to the text bridge past its owner.
    viewport_->setTextBridge(nullptr);
    if (services_.setWindowTitle) services_.setWindowTitle("Rivet");
}

// Child order below is the paint/hit-test order: tab strip, toolbar,
// sidebar, viewport, loading overlay, then the floating find bar and
// password prompt above the viewport, then the status bar.
void ShellController::buildWidgets() {
    // The theme is in place before any widget paints or caches a color.
    applyAppearance();
    if (services_.appearance != nullptr) services_.appearance->onChange([this] { applyAppearance(); });
    root_ = std::make_unique<ShellRoot>();

    // The viewport exists before the controllers (they reference it through
    // the context) but joins the tree after the sidebar.
    auto viewport = std::make_unique<ui::PdfViewport>();
    viewport_ = viewport.get();
    context_ = std::make_unique<ShellContext>(ShellContext{
        workspace_,
        services_,
        *viewport_,
        [this](std::string text) { setStatus(std::move(text)); },
        [this](ui::Widget* widget) { setFocus(widget); },
        [this] { layoutShell(); },
    });

    auto tabStrip = std::make_unique<ui::TabStrip>();
    tabStrip_ = tabStrip.get();
    tabStrip_->setOnTabActivated([this](std::size_t index) { workspace_.activateTab(index); });
    tabStrip_->setOnTabCloseRequested([this](std::size_t index) { requestCloseTab(index); });
    root_->addChild(std::move(tabStrip));

    auto toolbar = std::make_unique<ui::Toolbar>(ui::metrics::kToolbarHeight);
    toolbar_ = toolbar.get();
    root_->addChild(std::move(toolbar));
    buildToolbar();

    sidebar_ = std::make_unique<SidebarController>(*context_, *root_);
    auto splitter = std::make_unique<SidebarSplitter>([this](double width) { setSidebarWidth(width); });
    sidebarSplitter_ = splitter.get();
    splitter->setFrame(kHiddenFrame);
    root_->addChild(std::move(splitter));

    viewport_->setZoomChangedCallback([this](double zoom) { setZoomDisplay(zoom); });
    viewport_->setOnFocusRequested([this] { setFocus(nullptr); });
    // Current-page funnel: keeps the tab's tracked page, the sidebar
    // selection/reveal and the page field in sync.
    viewport_->setCurrentPageChangedCallback([this](std::size_t page) {
        if (DocumentTab* tab = workspace_.activeTab(); tab != nullptr) tab->setCurrentPage(page);
        sidebar_->setCurrentPage(page);
        statusBar_->updatePageIndicator();
    });
    root_->addChild(std::move(viewport));

    // Content slot of Markdown tabs: hidden (empty frame) unless a Markdown
    // tab is active (Rendered / Source / Split, see MarkdownHostView).
    MarkdownHostEnvironment markdownEnvironment;
    markdownEnvironment.dispatcher = services_.mainDispatcher;
    markdownEnvironment.scheduler = &scheduler_;
    markdownEnvironment.services = &services_;
    markdownEnvironment.setStatus = [this](std::string text) { setStatus(std::move(text)); };
    markdownEnvironment.setFocus = [this](ui::Widget* widget) { setFocus(widget); };
    auto markdownView = createMarkdownHostView(std::move(markdownEnvironment));
    markdownView_ = markdownView.get();
    markdownView_->setFrame(kHiddenFrame);
    root_->addChild(std::move(markdownView));

    // Content slot of comparison tabs (ADR-0029), hidden unless one is active.
    auto comparisonView = std::make_unique<ComparisonHostView>(
        ComparisonHostView::Environment{[this](ui::Widget* widget) { setFocus(widget); }});
    comparisonView_ = comparisonView.get();
    comparisonView_->setFrame(kHiddenFrame);
    root_->addChild(std::move(comparisonView));

    // Empty workspace: the welcome view (ADR-0033 §5).
    auto welcome = std::make_unique<WelcomeView>();
    welcome_ = welcome.get();
    welcome->setFrame(kHiddenFrame);
    welcome->setOnOpen([this] { handleOpenRequest(); });
    welcome->setOnOpenRecent([this](const std::filesystem::path& path) { openDocument(path); });
    root_->addChild(std::move(welcome));
    refreshWelcome();

    // Loading / error overlay, drawn above the viewport area.
    auto overlay = std::make_unique<TextLabel>("", ui::Font{14.0, ui::Font::Weight::Semibold},
                                               &ui::Palette::textSecondary, ui::TextAlign::Center);
    overlayLabel_ = overlay.get();
    root_->addChild(std::move(overlay));

    searchBar_ = std::make_unique<SearchBarController>(*context_, *root_);
    searchBar_->field().setLeadingIcon(ui::Icon::Search);
    searchBar_->setMarkdownTargetProvider(
        [this]() -> ISearchTarget* { return markdownView_ != nullptr ? markdownView_->searchTarget() : nullptr; });
    passwordPrompt_ = std::make_unique<PasswordPromptController>(*context_, *root_);
    statusBar_ = std::make_unique<StatusBarController>(
        *context_, *root_,
        engine_->isAvailable() ? "Ready" : "Ready — PDF support is not built into this binary");
    // The page indicator lives in the toolbar's page navigation.
    statusBar_->attachPageIndicator(*pageNavigator_);

    // Text interaction over the active tab.
    textInteraction_ = std::make_unique<TextInteractionController>(*context_);
    viewport_->setTextBridge(textInteraction_.get());

    // Page editing over the active tab (selection, thumbnails, crop tool).
    pageEditing_ = std::make_unique<PageEditingController>(*context_, *sidebar_, *statusBar_);

    // Annotation tools (above the viewport and the find bar: the note editor
    // floats over them) and their strip under the main toolbar.
    annotations_ = std::make_unique<AnnotationController>(*context_, *root_);
    annotationBar_ = std::make_unique<AnnotationBarController>(*context_, *root_, *annotations_);
    annotationBar_->setOnVisibilityChanged([this] { syncChrome(); });

    // Content editing (Edit / Add Text tools) and its properties strip. The
    // tools share one tool state with the annotation tools.
    content_ = std::make_unique<ContentController>(*context_, *root_, *annotations_, scheduler_,
                                                   createEditorContentBackend());
    contentBar_ = std::make_unique<ContentBarController>(*context_, *root_, *content_);
    content_->setOnStateChanged([this] {
        contentBar_->refresh();
        syncChrome();
    });

    // Form filling: its layer sits below the annotation layer (Select tool).
    forms_ = std::make_unique<FormController>(*context_, *root_, *annotations_);
    // Redaction marks (Redact tool, Apply Redactions): part of the content
    // tool state.
    redaction_ = std::make_unique<RedactionController>(*context_, *content_);
    ocr_ = std::make_unique<OcrController>(*context_, scheduler_);

    // Visual signatures (ADR-0025): the sheet floats above everything over
    // the viewport; Done arms the Sign tool with the chosen signature.
    signatureSheet_ = std::make_unique<SignatureSheetController>(*context_, *root_);
    signatureSheet_->setOnChosen([this](pdf::PdfSignatureAppearancePtr signature) {
        if (readyActiveTab() == nullptr) {
            setStatus("Signature saved — open a document to place it");
            return;
        }
        if (annotationBar_ != nullptr) annotationBar_->setVisible(true);
        annotations_->armSignature(std::move(signature));
        setStatus("Click on the page to place the signature, or drag to size it");
    });
    annotations_->setOnSignatureRequested([this] { openSignatureSheet(false); });

    // File lifecycle (save/save-as/import/merge/extract, dirty close/quit).
    fileLifecycle_ = std::make_unique<FileController>(
        *engine_, *context_, scheduler_, [this](std::string text) { setStatus(std::move(text)); },
        [this] {
            refreshTabStrip();
            updateWindowTitle();
        });
    fileLifecycle_->setSelectionProvider([this] {
        return pageEditing_ != nullptr && pageEditing_->activeSelection() != nullptr
                   ? pageEditing_->targetPages()
                   : std::vector<core::PageId>{};
    });
    fileLifecycle_->installLifecycleHandlers();

    // Markdown export / print (Export as PDF…, Print on a Markdown tab).
    markdownExport_ = std::make_unique<MarkdownExportController>(
        *engine_, *context_, scheduler_, printCoordinator_, [this](std::string text) { setStatus(std::move(text)); });
    markdownExport_->setImageSnapshotProvider([this](const DocumentTab& tab) {
        // The preview's decoded images, when it shows this tab.
        if (markdownView_ == nullptr || markdownView_->state() != tab.markdown() ||
            markdownView_->previewView() == nullptr) {
            return ExportImageMap{};
        }
        return markdownView_->previewView()->imageStore().snapshot();
    });
    fileLifecycle_->setMarkdownExporter(markdownExport_.get());

    // Workspace events.
    workspace_.setOnTabsChanged([this] { refreshTabStrip(); });
    workspace_.setOnActiveTabChanged([this] { bindActiveTab(); });

    // Context bars, then the floating popover menu and tooltip above all.
    buildContextBars();
    auto popup = std::make_unique<ui::PopupMenu>();
    popup_ = popup.get();
    popup->setFrame(kHiddenFrame);
    root_->addChild(std::move(popup));
    auto tooltip = std::make_unique<ui::Tooltip>(&delayedRunner_);
    tooltip_ = tooltip.get();
    tooltip->setFrame(kHiddenFrame);
    root_->addChild(std::move(tooltip));
    root_->onPointer = [this](const ui::PointerEvent& event, bool handled) { trackPointer(event, handled); };

    // Layout only once every controller exists (the controllers relayout the
    // shell through the context).
    root_->onLayout = [this] { layoutShell(); };
    layoutShell();
    refreshTabStrip();
    bindActiveTab();
}

// Toolbar (ADR-0033 §5). Leading: sidebar toggle, open, save. Center: the
// zoom control and page navigation (PDF) or the display-mode switch
// (Markdown). Trailing: the tool modes (PDF), then search, print and the
// overflow menu. Items are fixed-size; applyChromeForKind shows the items of
// the active tab's kind and syncChrome enables them from canPerform().
void ShellController::buildToolbar() {
    using Group = ui::Toolbar::Group;
    const auto addPdf = [this](std::unique_ptr<ui::Widget> item, double spacing, Group group) {
        pdfToolbarItems_.push_back(item.get());
        toolbar_->addItem(std::move(item), spacing, group);
    };
    const auto iconButton = [this](ui::Icon icon, const char* title, std::optional<AppCommand> command,
                                   std::function<void()> onClick) {
        auto button = std::make_unique<ui::IconButton>(
            icon, command.has_value() ? tooltipFor(title, *command) : std::string(title));
        button->setFrame(core::Rect{0.0, 0.0, ui::IconButton::kSize, ui::IconButton::kSize});
        button->setOnClick(std::move(onClick));
        if (command.has_value()) commandControls_.emplace_back(button.get(), *command);
        return button;
    };

    // Leading.
    auto sidebar = iconButton(ui::Icon::Sidebar, "Show or Hide the Sidebar", std::nullopt,
                              [this] { setSidebarVisible(!sidebarVisible_); });
    sidebarButton_ = sidebar.get();
    addPdf(std::move(sidebar), 4.0, Group::Leading);
    toolbar_->addItem(iconButton(ui::Icon::Open, "Open\xE2\x80\xA6", AppCommand::open(), [this] { handleOpenRequest(); }),
                      4.0, Group::Leading);
    auto save = iconButton(ui::Icon::Save, "Save", AppCommand::of(FileCommand::Save),
                           [this] { performFile(FileCommand::Save); });
    saveButton_ = save.get();
    toolbar_->addItem(std::move(save), 8.0, Group::Leading);

    // Center (PDF): zoom, then page navigation.
    auto zoom = std::make_unique<ui::ZoomControl>();
    zoomControl_ = zoom.get();
    zoom->setFrame(core::Rect{0.0, 0.0, ui::ZoomControl::kWidth, ui::metrics::kControlHeight});
    zoom->setOnZoomOut([this] { viewport_->zoomOutStep(); });
    zoom->setOnZoomIn([this] { viewport_->zoomInStep(); });
    zoom->setOnPercent([this](const core::Rect& part) { openZoomMenu(part); });
    addPdf(std::move(zoom), 20.0, Group::Center);
    const auto stepPage = [this](int delta) {
        DocumentTab* tab = readyActiveTab();
        if (tab == nullptr) return;
        const std::size_t count = tab->session()->pageCount();
        const long long next = static_cast<long long>(tab->currentPage()) + delta;
        if (next < 0 || next >= static_cast<long long>(count)) return;
        viewport_->goToPage(static_cast<std::size_t>(next));
    };
    auto previous = iconButton(ui::Icon::Previous, "Previous Page", std::nullopt, [stepPage] { stepPage(-1); });
    prevPageButton_ = previous.get();
    addPdf(std::move(previous), 2.0, Group::Center);
    auto navigator = std::make_unique<ui::Container>();
    pageNavigator_ = navigator.get();
    navigator->setFrame(core::Rect{0.0, 0.0, StatusBarController::kPageIndicatorWidth, ui::metrics::kControlHeight});
    addPdf(std::move(navigator), 0.0, Group::Center);
    auto next = iconButton(ui::Icon::Next, "Next Page", std::nullopt, [stepPage] { stepPage(+1); });
    nextPageButton_ = next.get();
    addPdf(std::move(next), 8.0, Group::Center);

    // Center (Markdown): Rendered | Source | Split.
    auto modes = std::make_unique<ui::SegmentedControl>(std::vector<ui::SegmentedControl::Segment>{
        {"Rendered", ui::Icon::MarkdownRendered,
         tooltipFor("Rendered", AppCommand::of(MarkdownDisplayMode::Rendered)), 40.0, true},
        {"Source", ui::Icon::MarkdownSource, tooltipFor("Source", AppCommand::of(MarkdownDisplayMode::Source)), 40.0,
         true},
        {"Split", ui::Icon::MarkdownSplit, tooltipFor("Split", AppCommand::of(MarkdownDisplayMode::Split)), 40.0,
         true}});
    markdownModes_ = modes.get();
    modes->setFrame(core::Rect{core::Point{}, modes->naturalSize()});
    modes->setOnSelect([this](std::size_t index) { performMarkdownMode(static_cast<MarkdownDisplayMode>(index)); });
    markdownToolbarItems_.push_back(modes.get());
    toolbar_->addItem(std::move(modes), 8.0, Group::Center);

    // Trailing: tool modes (PDF), then search, print, overflow.
    auto tools = std::make_unique<ui::SegmentedControl>(std::vector<ui::SegmentedControl::Segment>{
        {"View", {}, "Read and select text", 0.0, true},
        {"Annotate", {}, "Highlight, comment and draw", 0.0, true},
        {"Edit", {}, "Edit text and images", 0.0, true},
        {"Forms", {}, "Fill in form fields", 0.0, true},
        {"Redact", {}, "Mark content for redaction", 0.0, true}});
    toolModes_ = tools.get();
    tools->setFrame(core::Rect{core::Point{}, tools->naturalSize()});
    tools->setOnSelect([this](std::size_t index) { setToolMode(static_cast<ToolMode>(index)); });
    addPdf(std::move(tools), 12.0, Group::Trailing);

    auto search = iconButton(ui::Icon::Search, "Find", std::nullopt, [this] {
        if (chromeComparison_) return;
        if (!chromeMarkdown_ || markdownView_->searchTarget() != nullptr) searchBar_->toggle();
    });
    search->setTooltip(std::format("Find  {}", shortcutText(Shortcut{"f"})));
    searchButton_ = search.get();
    toolbar_->addItem(std::move(search), 4.0, Group::Trailing);
    auto print = iconButton(ui::Icon::Print, "Print\xE2\x80\xA6", std::nullopt, [this] { handlePrintRequest(); });
    print->setTooltip(std::format("Print\xE2\x80\xA6  {}", shortcutText(Shortcut{"p"})));
    printButton_ = print.get();
    toolbar_->addItem(std::move(print), 4.0, Group::Trailing);
    auto more = iconButton(ui::Icon::More, "More Commands", std::nullopt, [this] { openOverflowMenu(); });
    toolbar_->addItem(std::move(more), 0.0, Group::Trailing);

    for (ui::Widget* item : markdownToolbarItems_) toolbar_->setItemVisible(item, false);
}

// Context bars of the Forms and Redact modes and of Markdown tabs (the
// annotation and content bars have their own controllers).
void ShellController::buildContextBars() {
    constexpr double kButtonHeight = ui::metrics::kSmallControlHeight + 2.0;
    const auto addTextButton = [this](ui::ChromeBar& bar, double& x, const char* label, double width,
                                      std::optional<AppCommand> command, std::function<void()> onClick) {
        auto button = std::make_unique<ui::Button>(label);
        ui::Button* raw = button.get();
        button->setFrame(core::Rect{x, (ui::metrics::kContextBarHeight - kButtonHeight) / 2.0, width, kButtonHeight});
        button->setOnClick(std::move(onClick));
        if (command.has_value()) commandControls_.emplace_back(raw, *command);
        bar.addChild(std::move(button));
        x += width + 6.0;
        return raw;
    };

    auto forms = std::make_unique<ui::ChromeBar>(false, true);
    formsBar_ = forms.get();
    double x = 10.0;
    highlightFieldsButton_ =
        addTextButton(*forms, x, "Highlight Fields", 124.0, AppCommand::of(FormCommand::HighlightFields),
                      [this] { perform(AppCommand::of(FormCommand::HighlightFields)); });
    addTextButton(*forms, x, "Reset Form", 92.0, AppCommand::of(FormCommand::ResetForm),
                  [this] { perform(AppCommand::of(FormCommand::ResetForm)); });
    forms->setFrame(kHiddenFrame);
    root_->addChild(std::move(forms));

    auto redact = std::make_unique<ui::ChromeBar>(false, true);
    redactBar_ = redact.get();
    x = 10.0;
    addTextButton(*redact, x, "Mark Selection", 116.0, AppCommand::of(RedactCommand::MarkSelection),
                  [this] { perform(AppCommand::of(RedactCommand::MarkSelection)); });
    addTextButton(*redact, x, "Apply Redactions\xE2\x80\xA6", 136.0, AppCommand::of(RedactCommand::ApplyRedactions),
                  [this] { perform(AppCommand::of(RedactCommand::ApplyRedactions)); });
    addTextButton(*redact, x, "Clear Marks", 96.0, AppCommand::of(RedactCommand::ClearMarks),
                  [this] { perform(AppCommand::of(RedactCommand::ClearMarks)); });
    redact->setFrame(kHiddenFrame);
    root_->addChild(std::move(redact));

    // Markdown Format commands as icon buttons.
    auto format = std::make_unique<ui::ChromeBar>(false, true);
    formatBar_ = format.get();
    struct FormatItem {
        ui::Icon icon;
        const char* title;
        MarkdownFormatCommand command;
        double gapAfter;
    };
    const FormatItem items[] = {
        {ui::Icon::Bold, "Bold", MarkdownFormatCommand::Bold, 2.0},
        {ui::Icon::Italic, "Italic", MarkdownFormatCommand::Italic, 2.0},
        {ui::Icon::StrikeOut, "Strikethrough", MarkdownFormatCommand::Strikethrough, 2.0},
        {ui::Icon::Code, "Inline Code", MarkdownFormatCommand::Code, 2.0},
        {ui::Icon::Link, "Link", MarkdownFormatCommand::Link, 14.0},
        {ui::Icon::Heading, "Heading", MarkdownFormatCommand::Heading1, 14.0},
        {ui::Icon::BulletList, "Bulleted List", MarkdownFormatCommand::BulletList, 2.0},
        {ui::Icon::NumberedList, "Numbered List", MarkdownFormatCommand::NumberedList, 2.0},
        {ui::Icon::TaskList, "Task List", MarkdownFormatCommand::TaskList, 2.0},
        {ui::Icon::Quote, "Quote", MarkdownFormatCommand::Quote, 2.0},
    };
    x = 10.0;
    for (const FormatItem& item : items) {
        const AppCommand command = AppCommand::of(item.command);
        auto button = std::make_unique<ui::IconButton>(item.icon, tooltipFor(item.title, command));
        ui::IconButton* raw = button.get();
        button->setFrame(core::Rect{x, (ui::metrics::kContextBarHeight - 26.0) / 2.0, 26.0, 26.0});
        if (item.icon == ui::Icon::Heading) {
            // Paragraph / Heading 1-6 in a small menu.
            button->setTooltip("Paragraph and Headings");
            button->setOnClick([this, raw] {
                std::vector<ui::PopupMenu::Item> levels;
                const char* titles[] = {"Paragraph", "Heading 1", "Heading 2", "Heading 3",
                                        "Heading 4", "Heading 5", "Heading 6"};
                for (std::size_t i = 0; i < 7; ++i) {
                    const AppCommand level =
                        AppCommand::of(static_cast<MarkdownFormatCommand>(static_cast<std::size_t>(MarkdownFormatCommand::Paragraph) + i));
                    ui::PopupMenu::Item entry;
                    entry.title = titles[i];
                    entry.shortcut = shortcutText(Shortcut{std::to_string(i), true, false, true});
                    entry.enabled = canPerform(level);
                    entry.action = [this, level] { perform(level); };
                    levels.push_back(std::move(entry));
                }
                showPopup(std::move(levels), *raw, raw->bounds());
            });
            commandControls_.emplace_back(raw, command);
        } else {
            button->setOnClick([this, command] { perform(command); });
            commandControls_.emplace_back(raw, command);
        }
        format->addChild(std::move(button));
        x += 26.0 + item.gapAfter;
    }
    format->setFrame(kHiddenFrame);
    root_->addChild(std::move(format));
}

void ShellController::applyAppearance() {
    std::optional<ui::Color> accent;
    bool dark = false;
    if (services_.appearance != nullptr) {
        dark = services_.appearance->isDark();
        if (const auto platformAccent = services_.appearance->accentColor(); platformAccent.has_value()) {
            accent = ui::Color{platformAccent->r, platformAccent->g, platformAccent->b, 1.0};
        }
    }
    ui::setCurrentTheme(ui::Theme::make(dark, accent));
    // Widgets read tokens while painting: a repaint applies the new theme.
    if (root_ != nullptr) root_->invalidate();
}

void ShellController::layoutShell() {
    const core::Rect bounds = root_->bounds();
    const double width = bounds.size.width;
    const double height = bounds.size.height;
    // Markdown and comparison tabs have no sidebar, annotation or content
    // bars and no PDF viewport: their view fills the whole content area.
    const bool markdown = chromeMarkdown_ || chromeComparison_; // content-only chrome
    const bool empty = workspace_.tabCount() == 0;
    const bool pdfChrome = !markdown && !empty;
    core::Rect contentRect{0.0, 0.0, width, height};

    const auto hideContextBars = [this] {
        if (annotationBar_ != nullptr) annotationBar_->layout(kHiddenFrame);
        if (contentBar_ != nullptr) contentBar_->layout(kHiddenFrame);
        if (formsBar_ != nullptr) formsBar_->setFrame(kHiddenFrame);
        if (redactBar_ != nullptr) redactBar_->setFrame(kHiddenFrame);
        if (formatBar_ != nullptr) formatBar_->setFrame(kHiddenFrame);
    };

    if (presentationMode_) {
        // Chrome collapses to hidden frames; the viewport fills the window.
        tabStrip_->setFrame(kHiddenFrame);
        toolbar_->setFrame(kHiddenFrame);
        sidebar_->layout(kHiddenFrame);
        statusBar_->layout(kHiddenFrame);
        hideContextBars();
        if (sidebarSplitter_ != nullptr) sidebarSplitter_->setFrame(kHiddenFrame);
    } else {
        const double tabHeight = ui::metrics::kTabStripHeight;
        const double toolbarHeight = ui::metrics::kToolbarHeight;
        const double statusHeight = StatusBarController::kHeight;
        tabStrip_->setFrame(core::Rect{0.0, 0.0, width, tabHeight});
        toolbar_->setFrame(core::Rect{0.0, tabHeight, width, toolbarHeight});

        // One context bar under the toolbar for the active tool (or the
        // Markdown Format commands).
        double y = tabHeight + toolbarHeight;
        hideContextBars();
        const ToolMode mode = toolMode();
        const double barHeight = ui::metrics::kContextBarHeight;
        if (pdfChrome && annotationBar_ != nullptr && annotationBar_->visible()) {
            annotationBar_->layout(core::Rect{0.0, y, width, annotationBar_->height()});
            y += annotationBar_->height();
        } else if (pdfChrome && contentBar_ != nullptr && contentBar_->visible()) {
            contentBar_->layout(core::Rect{0.0, y, width, contentBar_->height()});
            y += contentBar_->height();
        } else if (pdfChrome && mode == ToolMode::Forms && formsBar_ != nullptr) {
            formsBar_->setFrame(core::Rect{0.0, y, width, barHeight});
            y += barHeight;
        } else if (pdfChrome && mode == ToolMode::Redact && redactBar_ != nullptr) {
            redactBar_->setFrame(core::Rect{0.0, y, width, barHeight});
            y += barHeight;
        } else if (chromeMarkdown_ && formatBar_ != nullptr) {
            formatBar_->setFrame(core::Rect{0.0, y, width, barHeight});
            y += barHeight;
        }
        toolbar_->setBottomHairline(y == tabHeight + toolbarHeight);

        const double middleTop = y;
        const double middleHeight = std::max(0.0, height - middleTop - statusHeight);
        const double sidebarWidth =
            (pdfChrome && sidebarVisible_) ? std::min(sidebarWidth_, std::max(0.0, width - 240.0)) : 0.0;
        sidebar_->layout(sidebarWidth > 0.0 ? core::Rect{0.0, middleTop, sidebarWidth, middleHeight} : kHiddenFrame);
        if (sidebarSplitter_ != nullptr) {
            sidebarSplitter_->setFrame(sidebarWidth > 0.0 ? core::Rect{sidebarWidth - 3.0, middleTop, 6.0, middleHeight}
                                                          : kHiddenFrame);
        }
        statusBar_->layout(core::Rect{0.0, height - statusHeight, width, statusHeight});
        contentRect = core::Rect{sidebarWidth, middleTop, std::max(0.0, width - sidebarWidth), middleHeight};
    }
    viewport_->setFrame(markdown || (empty && !presentationMode_) ? kHiddenFrame : contentRect);
    markdownView_->setFrame(chromeMarkdown_ ? contentRect : kHiddenFrame);
    comparisonView_->setFrame(chromeComparison_ ? contentRect : kHiddenFrame);
    if (welcome_ != nullptr) welcome_->setFrame(empty && !presentationMode_ ? contentRect : kHiddenFrame);
    // Everything over the content follows its frame.
    overlayLabel_->setFrame(contentRect);
    searchBar_->layout(markdown ? contentRect : viewport_->frame());
    passwordPrompt_->layout(viewport_->frame());
    if (annotations_ != nullptr) annotations_->layout(viewport_->frame());
    if (content_ != nullptr) content_->layout(viewport_->frame());
    if (forms_ != nullptr) forms_->layout(viewport_->frame());
    if (signatureSheet_ != nullptr) signatureSheet_->layout(markdown ? contentRect : viewport_->frame());
    if (tooltip_ != nullptr) tooltip_->dismiss();
}

void ShellController::refreshTabStrip() {
    std::vector<ui::TabStrip::Tab> tabs;
    tabs.reserve(workspace_.tabCount());
    for (std::size_t i = 0; i < workspace_.tabCount(); ++i) {
        tabs.push_back(ui::TabStrip::Tab{workspace_.tab(i)->title(), workspace_.tab(i)->isDirty()});
    }
    const std::optional<std::size_t> active =
        workspace_.activeIndex() == DocumentWorkspace::kNoTab
            ? std::nullopt
            : std::optional<std::size_t>{workspace_.activeIndex()};
    tabStrip_->setTabs(std::move(tabs), active);
    viewport_->invalidate();
    updateDocumentEdited();
    // The welcome view appears / disappears with the last / first tab.
    const bool showWelcome = workspace_.tabCount() == 0;
    if (welcome_ != nullptr && showWelcome != !welcome_->frame().isEmpty()) {
        refreshWelcome();
        layoutShell();
        applyChromeForKind(chromeMarkdown_, chromeComparison_);
    }
    syncChrome();
}

void ShellController::updateDocumentEdited() {
    if (services_.setDocumentEdited == nullptr) return;
    const DocumentTab* tab = workspace_.activeTab();
    services_.setDocumentEdited(tab != nullptr && tab->isDirty());
}

void ShellController::applyChromeForKind(bool markdown, bool comparison) {
    // The empty window shows only the leading file commands (welcome view).
    const bool empty = workspace_.tabCount() == 0;
    for (ui::Widget* item : pdfToolbarItems_) toolbar_->setItemVisible(item, !markdown && !comparison && !empty);
    for (ui::Widget* item : markdownToolbarItems_) toolbar_->setItemVisible(item, markdown);
    syncChrome();
    if (chromeMarkdown_ == markdown && chromeComparison_ == comparison) return;
    chromeMarkdown_ = markdown;
    chromeComparison_ = comparison;
    layoutShell();
}

void ShellController::syncModeButtons() {
    const std::optional<MarkdownDisplayMode> mode = activeMarkdownMode();
    if (markdownModes_ != nullptr) {
        markdownModes_->setSelected(mode.has_value() ? std::optional<std::size_t>(static_cast<std::size_t>(*mode))
                                                     : std::nullopt);
    }
    syncChrome();
}

// Enables every command-bound control from canPerform() (disabled controls
// stay visible, ADR-0033 §4) and refreshes the checked/active states.
void ShellController::syncChrome() {
    if (context_ == nullptr) return; // still building
    for (const auto& [widget, command] : commandControls_) {
        const bool enabled = canPerform(command);
        if (auto* icon = dynamic_cast<ui::IconButton*>(widget); icon != nullptr) icon->setEnabled(enabled);
        if (auto* button = dynamic_cast<ui::Button*>(widget); button != nullptr) button->setEnabled(enabled);
    }
    const bool pdfReady = readyActiveTab() != nullptr;
    if (zoomControl_ != nullptr) zoomControl_->setEnabled(pdfReady);
    if (prevPageButton_ != nullptr) prevPageButton_->setEnabled(pdfReady);
    if (nextPageButton_ != nullptr) nextPageButton_->setEnabled(pdfReady);
    if (sidebarButton_ != nullptr) {
        sidebarButton_->setEnabled(pdfReady);
        sidebarButton_->setActive(pdfReady && sidebarVisible_);
    }
    const bool markdownReady = context_->readyMarkdownTab() != nullptr;
    if (searchButton_ != nullptr) {
        searchButton_->setEnabled(pdfReady || (markdownReady && markdownView_->searchTarget() != nullptr));
    }
    if (printButton_ != nullptr) printButton_->setEnabled(pdfReady || markdownReady);
    if (toolModes_ != nullptr) {
        for (std::size_t i = 0; i < toolModes_->segmentCount(); ++i) toolModes_->setSegmentEnabled(i, pdfReady);
    }
    if (highlightFieldsButton_ != nullptr && forms_ != nullptr) {
        highlightFieldsButton_->setActive(forms_->highlightFields());
    }
    syncToolMode();
}

ToolMode ShellController::toolMode() const {
    if (redaction_ != nullptr && redaction_->isChecked(RedactCommand::ToolRedact).value_or(false)) {
        return ToolMode::Redact;
    }
    if (content_ != nullptr &&
        (content_->tool() == ContentTool::SelectObject || content_->tool() == ContentTool::AddText)) {
        return ToolMode::Edit;
    }
    if (annotationBar_ != nullptr && annotationBar_->visible()) return ToolMode::Annotate;
    return formsMode_ ? ToolMode::Forms : ToolMode::View;
}

void ShellController::syncToolMode() {
    // A controller-driven tool (menu, shortcut) ends the Forms mode.
    if (formsMode_) {
        formsMode_ = false;
        formsMode_ = toolMode() == ToolMode::View; // still Forms unless another tool took over
    }
    const ToolMode mode = toolMode();
    if (toolModes_ == nullptr) return;
    const std::optional<std::size_t> selected =
        readyActiveTab() != nullptr ? std::optional<std::size_t>(static_cast<std::size_t>(mode)) : std::nullopt;
    if (toolModes_->selected() != selected) {
        toolModes_->setSelected(selected);
        layoutShell(); // the context bar follows the mode
    }
}

void ShellController::setToolMode(ToolMode mode) {
    if (readyActiveTab() == nullptr) return;
    // Leave the current tool (hiding the annotation strip returns to the
    // Select tool and ContentTool::None), then enter the new one.
    if (mode != ToolMode::Annotate && annotationBar_->visible()) annotationBar_->setVisible(false);
    if (mode != ToolMode::Edit && mode != ToolMode::Redact && content_->tool() != ContentTool::None) {
        content_->setTool(ContentTool::None);
    }
    formsMode_ = mode == ToolMode::Forms;
    switch (mode) {
    case ToolMode::View: break;
    case ToolMode::Annotate: annotationBar_->setVisible(true); break;
    case ToolMode::Edit:
        if (content_->tool() != ContentTool::SelectObject && content_->tool() != ContentTool::AddText) {
            content_->setTool(ContentTool::SelectObject);
        }
        break;
    case ToolMode::Forms:
        if (!forms_->highlightFields() && forms_->canPerform(FormCommand::HighlightFields)) {
            forms_->perform(FormCommand::HighlightFields);
        }
        break;
    case ToolMode::Redact:
        if (!redaction_->isChecked(RedactCommand::ToolRedact).value_or(false)) {
            redaction_->perform(RedactCommand::ToolRedact);
        }
        break;
    }
    syncChrome();
    layoutShell();
}

void ShellController::setSidebarVisible(bool visible) {
    if (sidebarVisible_ == visible) return;
    sidebarVisible_ = visible;
    syncChrome();
    layoutShell();
}

void ShellController::setSidebarWidth(double width) {
    const double clamped = std::clamp(width, ui::metrics::kSidebarMinWidth, ui::metrics::kSidebarMaxWidth);
    if (clamped == sidebarWidth_) return;
    sidebarWidth_ = clamped;
    layoutShell();
}

std::string ShellController::tooltipFor(const char* title, const AppCommand& command) const {
    for (const Menu& menu : buildMenuBar()) {
        for (const MenuItem& item : menu.items) {
            if (item.command == command && item.shortcut.has_value()) {
                return std::format("{}  {}", title, shortcutText(*item.shortcut));
            }
        }
    }
    return title;
}

// Every menu-bar command (the overflow "more" menu, ADR-0033 §5): one
// submenu per menu, items enabled/checked from the shell.
std::vector<ui::PopupMenu::Item> ShellController::overflowItems() const {
    std::vector<ui::PopupMenu::Item> menus;
    for (const Menu& menu : buildMenuBar()) {
        ui::PopupMenu::Item top;
        top.title = menu.title;
        for (const MenuItem& item : menu.items) {
            if (item.role == MenuRole::Quit && !canPerform(item.command)) continue;
            ui::PopupMenu::Item entry;
            entry.title = item.title;
            if (item.shortcut.has_value()) entry.shortcut = shortcutText(*item.shortcut);
            entry.enabled = canPerform(item.command);
            entry.checked = isChecked(item.command).value_or(false);
            entry.separatorBefore = item.separatorBefore;
            const AppCommand command = item.command;
            ShellController* self = const_cast<ShellController*>(this);
            entry.action = [self, command] { self->perform(command); };
            top.submenu.push_back(std::move(entry));
        }
        if (!top.submenu.empty()) menus.push_back(std::move(top));
    }
    return menus;
}

void ShellController::showPopup(std::vector<ui::PopupMenu::Item> items, ui::Widget& anchorWidget,
                                const core::Rect& localAnchor) {
    if (popup_ == nullptr) return;
    if (tooltip_ != nullptr) tooltip_->dismiss();
    const core::Point origin = anchorWidget.originInRoot();
    popup_->open(std::move(items), core::Rect{origin + localAnchor.origin, localAnchor.size}, root_->bounds());
}

void ShellController::openOverflowMenu() {
    // Anchored under the last toolbar item (the "more" button).
    const auto& items = toolbar_->children();
    if (items.empty()) return;
    ui::Widget& more = *items.back();
    std::vector<ui::PopupMenu::Item> menu = overflowItems();
    showPopup(std::move(menu), more, more.bounds());
}

void ShellController::openZoomMenu(const core::Rect& anchor) {
    if (zoomControl_ == nullptr || readyActiveTab() == nullptr) return;
    std::vector<ui::PopupMenu::Item> items;
    const auto add = [&items](std::string title, std::function<void()> action, bool separator = false) {
        ui::PopupMenu::Item item;
        item.title = std::move(title);
        item.action = std::move(action);
        item.separatorBefore = separator;
        items.push_back(std::move(item));
    };
    add("Actual Size", [this] { viewport_->zoomActualSize(); });
    items.back().shortcut = shortcutText(Shortcut{"0"});
    add("Fit Width", [this] { viewport_->setFitMode(render::ZoomState::FitMode::Width); });
    add("Fit Page", [this] { viewport_->setFitMode(render::ZoomState::FitMode::Page); });
    const double current = viewport_->zoom().zoom();
    bool first = true;
    for (const int percent : {50, 75, 100, 125, 150, 200, 300, 400}) {
        const double value = percent / 100.0;
        add(std::format("{}%", percent), [this, value] { viewport_->setManualZoom(value); }, first);
        items.back().checked = std::fabs(current - value) < 0.005;
        first = false;
    }
    showPopup(std::move(items), *zoomControl_, anchor);
}

void ShellController::refreshWelcome() {
    if (welcome_ == nullptr) return;
    welcome_->setRecentFiles(recentFiles_.entries());
}

void ShellController::trackPointer(const ui::PointerEvent& event, bool handled) {
    (void)handled;
    if (tooltip_ == nullptr) return;
    switch (event.type) {
    case ui::PointerEventType::Down:
    case ui::PointerEventType::Scroll: tooltip_->dismiss(); return;
    case ui::PointerEventType::Up: return;
    case ui::PointerEventType::Move:
    case ui::PointerEventType::Entered:
    case ui::PointerEventType::Exited: break;
    }
    if (event.button != 0) return; // a drag: no hover changes
    const ui::Widget* target =
        event.type == ui::PointerEventType::Exited ? nullptr : root_->hitTest(event.position);
    if (target == tooltip_ || target == root_.get()) target = target == tooltip_ ? hoveredWidget_ : nullptr;
    if (target != hoveredWidget_) {
        // Tell the widget the pointer left: widgets only see moves inside
        // their own frame, so without this a hover state would stick.
        if (hoveredWidget_ != nullptr) {
            std::vector<const ui::Widget*> stack{root_.get()};
            bool alive = false;
            while (!stack.empty() && !alive) {
                const ui::Widget* w = stack.back();
                stack.pop_back();
                if (w == hoveredWidget_) {
                    alive = true;
                    break;
                }
                for (const auto& child : w->children()) stack.push_back(child.get());
            }
            if (alive) {
                auto* previous = const_cast<ui::Widget*>(hoveredWidget_);
                ui::PointerEvent exited = event;
                exited.type = ui::PointerEventType::Exited;
                exited.position = event.position - previous->originInRoot();
                previous->onMouse(exited);
            }
        }
        hoveredWidget_ = target;
    }
    // The tooltip of the hovered widget (or of the nearest ancestor with one).
    const ui::Widget* tipOwner = target;
    while (tipOwner != nullptr && tipOwner->tooltip().empty()) tipOwner = tipOwner->parent();
    if (tipOwner == root_.get()) tipOwner = nullptr;
    if (tipOwner == nullptr) {
        tooltip_->dismiss();
        return;
    }
    const core::Rect anchor{tipOwner->originInRoot(), tipOwner->frame().size};
    tooltip_->hoverTarget(tipOwner, tipOwner->tooltip(), anchor, root_->bounds());
}

void ShellController::DelayedRunner::runAfter(std::chrono::milliseconds delay, std::function<void()> task) {
    core::IMainThreadDispatcher* dispatcher = shell_.services_.mainDispatcher;
    if (dispatcher == nullptr) return; // tests without a dispatcher: no tooltips
    std::optional<core::AsyncScope::Token> token = shell_.delayScope_.enter();
    if (!token) return;
    const auto shared = std::make_shared<core::AsyncScope::Token>(std::move(*token));
    const std::weak_ptr<int> alive = shell_.alive_;
    shell_.scheduler_.post([shared, alive, dispatcher, delay, task = std::move(task)]() mutable {
        const auto until = std::chrono::steady_clock::now() + delay;
        while (std::chrono::steady_clock::now() < until && !shared->cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (shared->cancelled()) return;
        dispatcher->post([alive, task = std::move(task)] {
            if (alive.lock()) task();
        });
    });
}

std::optional<MarkdownDisplayMode> ShellController::activeMarkdownMode() const {
    const DocumentTab* tab = context_ != nullptr ? context_->readyMarkdownTab() : nullptr;
    if (tab == nullptr) return std::nullopt;
    return tab->markdown()->mode();
}

void ShellController::performMarkdownMode(MarkdownDisplayMode mode) {
    if (DocumentTab* tab = context_->readyMarkdownTab(); tab != nullptr) tab->markdown()->setMode(mode);
}

bool ShellController::canPerformMarkdownMode() const {
    return context_ != nullptr && context_->readyMarkdownTab() != nullptr;
}

// A Markdown tab's state changed (edit, undo/redo, mode, save): repaint the
// view when it is the active tab, always refresh the dirty markers.
void ShellController::onMarkdownChanged(TabId tab) {
    refreshTabStrip();
    const DocumentTab* active = workspace_.activeTab();
    if (active == nullptr || active->id() != tab) return;
    syncModeButtons();
    markdownView_->stateChanged();
}

// Rebinds every view to the active tab. Loading / error / password states
// appear inside this tab's view, never replacing other documents.
void ShellController::bindActiveTab() {
    DocumentTab* tab = workspace_.activeTab();
    overlayLabel_->setText("");
    passwordPrompt_->bindTab(tab);

    // Chrome follows the tab's kind (also while it loads or failed to open).
    const bool markdown = tab != nullptr && tab->isMarkdown();
    const bool comparison = tab != nullptr && tab->isComparison();
    // Detach the PDF viewport from the previous tab's view state BEFORE the
    // relayout hides it: a zero-size layout must not rewrite that tab's zoom.
    if (markdown || comparison) viewport_->clearDocument();
    applyChromeForKind(markdown, comparison);
    if (comparison) {
        markdownView_->bind(nullptr);
        bindComparisonTab(tab);
        return;
    }
    comparisonView_->bind(nullptr);
    if (markdown) {
        bindMarkdownTab(tab);
        return;
    }
    markdownView_->bind(nullptr);

    if (tab == nullptr || tab->state() != DocumentTab::State::Ready) {
        viewport_->clearDocument();
        sidebar_->bindTab(nullptr);
        pageEditing_->bindTab(nullptr);
        annotations_->bindTab(nullptr);
        content_->bindTab(nullptr);
        forms_->bindTab(nullptr);
        redaction_->bindTab(nullptr);
        if (tab == nullptr) {
            setStatus("No document open");
            setZoomDisplay(viewport_->zoom().zoom());
        } else if (tab->state() == DocumentTab::State::Loading) {
            overlayLabel_->setText(std::format("Loading {}…", tab->title()));
            setStatus(std::format("Loading {}…", tab->title()));
        } else if (tab->state() == DocumentTab::State::Error) {
            overlayLabel_->setText(std::format("Could not open {} — {}", tab->title(), tab->errorText()));
            setStatus(std::format("Failed to open {}: {}", tab->title(), tab->errorText()));
        } else { // NeedsPassword: the prompt (bound above) has focus
            overlayLabel_->setText(std::format("{} is password protected", tab->title()));
            setStatus(std::format("{} requires a password", tab->title()));
        }
        statusBar_->updatePageIndicator();
        updateWindowTitle();
        return;
    }

    editor::DocumentSession* session = tab->session();
    // Dirty flips (edit, undo back to saved, save) refresh the tab marker and
    // the window's edited dot.
    session->setOnDirtyChanged([this](bool) { refreshTabStrip(); });
    viewport_->setDocument(session->id(), &session->layout(), &session->renderSource(),
                           [session] { return session->revision(); }, &tab->viewState());
    // Thumbnails, page labels, outline and the restored current page.
    sidebar_->bindTab(tab);
    pageEditing_->bindTab(tab);
    annotations_->bindTab(tab);
    content_->bindTab(tab);
    forms_->bindTab(tab);
    redaction_->bindTab(tab);

    // First bind of a fresh tab: open fit-to-width (Phase 1 behavior). The
    // mode stays active (resizes recompute the zoom) until the user zooms.
    if (!tab->viewStateInitialized()) {
        tab->markViewStateInitialized();
        viewport_->setFitMode(render::ZoomState::FitMode::Width);
    }

    statusBar_->updatePageIndicator();
    setZoomDisplay(viewport_->zoom().zoom());
    setStatus(std::format("{} — {} page{}", tab->title(), session->pageCount(),
                          session->pageCount() == 1 ? "" : "s"));
    updateWindowTitle();

    // Per-tab text interaction: selection repaints the view; the search
    // drives the find bar (which closes: a different tab, a different search).
    textInteraction_->bindTab(*tab);
    searchBar_->bindTab(*tab);
    viewport_->invalidate();
}

// A Markdown tab owns the content area: every PDF binding is dropped (no
// stale session/selection/search reaches the hidden PDF chrome) and the host
// view is bound to the tab's state once it is Ready.
void ShellController::bindMarkdownTab(DocumentTab* tab) {
    viewport_->clearDocument();
    sidebar_->bindTab(nullptr);
    pageEditing_->bindTab(nullptr);
    annotations_->bindTab(nullptr);
    content_->bindTab(nullptr);
    forms_->bindTab(nullptr);
    redaction_->bindTab(nullptr);
    searchBar_->bindMarkdownTab();
    setPresentationMode(false);

    if (tab->state() == DocumentTab::State::Ready && tab->markdown() != nullptr) {
        MarkdownTabState* state = tab->markdown();
        const TabId id = tab->id();
        state->setOnChanged([this, id] { onMarkdownChanged(id); });
        markdownView_->bind(state);
        setStatus(std::format("{} — Markdown ({})", tab->title(),
                              state->lineEnding() == LineEnding::CRLF ? "CRLF" : "LF"));
    } else {
        markdownView_->bind(nullptr);
        if (tab->state() == DocumentTab::State::Loading) {
            overlayLabel_->setText(std::format("Loading {}…", tab->title()));
            setStatus(std::format("Loading {}…", tab->title()));
        } else {
            overlayLabel_->setText(std::format("Could not open {} — {}", tab->title(), tab->errorText()));
            setStatus(std::format("Failed to open {}: {}", tab->title(), tab->errorText()));
        }
    }
    syncModeButtons();
    statusBar_->updatePageIndicator();
    updateWindowTitle();
    updateDocumentEdited();
    markdownView_->invalidate();
}

// A comparison tab owns the content area like a Markdown tab: every PDF
// binding is dropped and the comparison view shows the tab's state (opening,
// comparing, failed or the two documents with their differences).
void ShellController::bindComparisonTab(DocumentTab* tab) {
    viewport_->clearDocument();
    sidebar_->bindTab(nullptr);
    pageEditing_->bindTab(nullptr);
    annotations_->bindTab(nullptr);
    content_->bindTab(nullptr);
    forms_->bindTab(nullptr);
    redaction_->bindTab(nullptr);
    searchBar_->bindMarkdownTab();
    setPresentationMode(false);

    ComparisonTabState* state = tab->comparison();
    const TabId id = tab->id();
    state->setOnChanged([this, id] {
        const DocumentTab* active = workspace_.activeTab();
        if (active == nullptr || active->id() != id) return;
        comparisonView_->refresh();
        if (const ComparisonTabState* current = active->comparison(); current != nullptr) {
            if (current->phase() == ComparisonTabState::Phase::Failed) {
                setStatus(std::format("Comparison failed: {}", current->errorText()));
            } else if (current->phase() == ComparisonTabState::Phase::Ready && current->result() != nullptr) {
                const std::size_t count = current->result()->changes.size();
                setStatus(count == 0 ? std::string("No differences found")
                                     : std::format("{} difference{}", count, count == 1 ? "" : "s"));
            }
        }
    });
    comparisonView_->bind(state);
    setStatus(std::format("{} \u2014 comparison", tab->title()));
    statusBar_->updatePageIndicator();
    updateWindowTitle();
    updateDocumentEdited();
}

bool ShellController::canPerformCompare() const {
    const DocumentTab* tab = context_ != nullptr ? context_->readyPdfTab() : nullptr;
    return tab != nullptr && engine_->isAvailable() && services_.fileDialog != nullptr &&
           services_.mainDispatcher != nullptr;
}

void ShellController::performCompare() {
    if (!canPerformCompare()) return;
    const DocumentTab* tab = context_->readyPdfTab();
    if (tab == nullptr || tab->session() == nullptr) return; // canPerformCompare() guarantees both
    // Both sides are read from disk: the left one is the active document as
    // last saved (ADR-0029), the right one is picked here.
    const std::filesystem::path leftPath = tab->session()->path();
    const bool unsaved = tab->isDirty();
    const core::Result<std::filesystem::path> chosen = services_.fileDialog->openPdf();
    if (!chosen.has_value()) {
        if (chosen.error().code != core::ErrorCode::Cancelled) {
            setStatus("Could not choose a file: " + core::describe(chosen.error()));
        }
        return;
    }
    ComparisonTabState::Environment environment{engine_.get(), &scheduler_, services_.mainDispatcher};
    std::unique_ptr<ComparisonTabState> state = ComparisonTabState::start(environment, leftPath, *chosen);
    workspace_.openComparisonTab(std::format("{} vs {}", leftPath.filename().string(), chosen->filename().string()),
                                 std::move(state));
    if (unsaved) setStatus("Comparing the saved version of the active document (unsaved edits are not included)");
}

void ShellController::handleOpenRequest() {
    if (services_.fileDialog == nullptr) {
        setStatus("No file dialog available on this platform backend");
        return;
    }
    const core::Result<std::filesystem::path> chosen = services_.fileDialog->openDocument();
    if (!chosen.has_value()) {
        if (chosen.error().code != core::ErrorCode::Cancelled) {
            setStatus("Could not choose a file: " + core::describe(chosen.error()));
        }
        return;
    }
    openDocument(*chosen);
}

void ShellController::openDocument(const std::filesystem::path& path) {
    // Availability is per kind: Markdown never needs the PDF engine.
    if (documentKindForPath(path) == DocumentKind::Pdf && !engine_->isAvailable()) {
        setStatus("PDF support is not built into this binary (RIVET_WITH_PDFIUM=OFF)");
        return;
    }
    // Recent files (welcome view): paths only, existing files only.
    if (std::error_code error; std::filesystem::is_regular_file(path, error)) {
        recentFiles_.add(path);
        refreshWelcome();
    }
    // Asynchronous: the loading tab appears immediately; bindActiveTab shows
    // its state. Completing fires onTabsChanged / onActiveTabChanged.
    workspace_.openDocument(path);
}

void ShellController::performPageEdit(PageEditCommand command) {
    if (DocumentTab* tab = context_ != nullptr ? context_->readyMarkdownTab() : nullptr; tab != nullptr) {
        // Markdown: Undo/Redo drive the tab's text command stack; page
        // commands do not exist there.
        if (command != PageEditCommand::Undo && command != PageEditCommand::Redo) {
            setStatus("Page commands are only available for PDF documents");
            return;
        }
        if (tab->markdown()->isEditingLocked()) {
            setStatus("A save is in progress — editing is paused");
        } else if (command == PageEditCommand::Undo) {
            if (!tab->undo()) setStatus("Nothing to undo");
        } else if (!tab->redo()) {
            setStatus("Nothing to redo");
        }
        return;
    }
    if (pageEditing_ != nullptr) pageEditing_->perform(command);
}

bool ShellController::canPerformPageEdit(PageEditCommand command) const {
    if (const DocumentTab* tab = context_ != nullptr ? context_->readyMarkdownTab() : nullptr; tab != nullptr) {
        if (command == PageEditCommand::Undo) return tab->canUndo();
        if (command == PageEditCommand::Redo) return tab->canRedo();
        return false;
    }
    return pageEditing_ != nullptr && pageEditing_->canPerform(command);
}

void ShellController::openSignatureSheet(bool manage) {
    if (signatureSheet_ == nullptr || annotations_ == nullptr) return;
    if (!manage && readyActiveTab() == nullptr) {
        setStatus("Open a document to sign");
        return;
    }
    signatureSheet_->setInkColor(annotations_->toolStyle(AnnotationTool::Sign).style.color);
    signatureSheet_->open(manage);
}

void ShellController::performAnnotation(AnnotationCommand command) {
    if (annotations_ == nullptr) return;
    if (command == AnnotationCommand::Sign || command == AnnotationCommand::ManageSignatures) {
        openSignatureSheet(command == AnnotationCommand::ManageSignatures);
        return;
    }
    // A tool chosen from the menu reveals the strip that shows it.
    if (static_cast<std::size_t>(command) <= static_cast<std::size_t>(AnnotationCommand::ToolStamp) &&
        annotationBar_ != nullptr &&
        command != AnnotationCommand::ToolSelect && readyActiveTab() != nullptr) {
        annotationBar_->setVisible(true);
    }
    annotations_->perform(command);
}

bool ShellController::canPerformAnnotation(AnnotationCommand command) const {
    if (command == AnnotationCommand::Sign) {
        return signatureSheet_ != nullptr && context_ != nullptr && context_->readyActiveTab() != nullptr;
    }
    if (command == AnnotationCommand::ManageSignatures) return signatureSheet_ != nullptr;
    return annotations_ != nullptr && annotations_->canPerform(command);
}

void ShellController::performContent(ContentCommand command) {
    if (command == ContentCommand::RecognizeTextMissing || command == ContentCommand::RecognizeTextAll) {
        if (ocr_ != nullptr) ocr_->perform(command);
        return;
    }
    if (content_ != nullptr) content_->perform(command);
}

bool ShellController::canPerformContent(ContentCommand command) const {
    if (command == ContentCommand::RecognizeTextMissing || command == ContentCommand::RecognizeTextAll) {
        return ocr_ != nullptr && ocr_->canPerform(command);
    }
    return content_ != nullptr && content_->canPerform(command);
}

void ShellController::performFile(FileCommand command) {
    if (command == FileCommand::Compare) {
        performCompare();
        return;
    }
    if (fileLifecycle_ != nullptr) fileLifecycle_->perform(command);
}

bool ShellController::canPerformFile(FileCommand command) const {
    if (command == FileCommand::Compare) return canPerformCompare();
    return fileLifecycle_ != nullptr && fileLifecycle_->canPerform(command);
}

void ShellController::perform(const AppCommand& command) {
    switch (command.kind) {
    case AppCommand::Kind::Open: handleOpenRequest(); break;
    case AppCommand::Kind::Quit:
        if (quitHandler_) quitHandler_();
        break;
    case AppCommand::Kind::File: performFile(command.file()); break;
    case AppCommand::Kind::PageEdit: performPageEdit(command.pageEdit()); break;
    case AppCommand::Kind::Annotation: performAnnotation(command.annotation()); break;
    case AppCommand::Kind::Content: performContent(command.content()); break;
    case AppCommand::Kind::MarkdownMode: performMarkdownMode(command.markdownMode()); break;
    case AppCommand::Kind::MarkdownFormat:
        if (context_ != nullptr && context_->readyMarkdownTab() != nullptr) {
            markdownView_->applyFormat(command.markdownFormat());
        }
        break;
    case AppCommand::Kind::Form:
        if (forms_ != nullptr) forms_->perform(command.form());
        break;
    case AppCommand::Kind::Redact:
        if (redaction_ != nullptr) redaction_->perform(command.redact());
        break;
    }
}

bool ShellController::canPerform(const AppCommand& command) const {
    switch (command.kind) {
    case AppCommand::Kind::Open: return true;
    case AppCommand::Kind::Quit: return static_cast<bool>(quitHandler_);
    case AppCommand::Kind::File: return canPerformFile(command.file());
    case AppCommand::Kind::PageEdit: return canPerformPageEdit(command.pageEdit());
    case AppCommand::Kind::Annotation: return canPerformAnnotation(command.annotation());
    case AppCommand::Kind::Content: return canPerformContent(command.content());
    case AppCommand::Kind::MarkdownMode: return canPerformMarkdownMode();
    case AppCommand::Kind::MarkdownFormat: {
        const DocumentTab* tab = context_ != nullptr ? context_->readyMarkdownTab() : nullptr;
        return tab != nullptr && !tab->markdown()->isEditingLocked();
    }
    case AppCommand::Kind::Form: return forms_ != nullptr && forms_->canPerform(command.form());
    case AppCommand::Kind::Redact: return redaction_ != nullptr && redaction_->canPerform(command.redact());
    }
    return false;
}

std::optional<bool> ShellController::isChecked(const AppCommand& command) const {
    if (command.kind == AppCommand::Kind::Form) {
        return forms_ != nullptr ? forms_->isChecked(command.form()) : std::nullopt;
    }
    if (command.kind == AppCommand::Kind::Redact) {
        return redaction_ != nullptr ? redaction_->isChecked(command.redact()) : std::nullopt;
    }
    if (command.kind != AppCommand::Kind::MarkdownMode) return std::nullopt;
    const auto mode = activeMarkdownMode();
    return mode.has_value() && *mode == command.markdownMode();
}

void ShellController::requestCloseTab(std::size_t index) {
    cancelPrintForTab(index);
    if (ocr_ != nullptr) {
        if (DocumentTab* closing = workspace_.tab(index); closing != nullptr) ocr_->cancelForTab(closing->id());
    }
    DocumentTab* tab = workspace_.tab(index);
    if (tab != nullptr && fileLifecycle_ != nullptr) {
        // The prompt is modal and the controller may close the tab itself
        // (Don't Save) or run completions that shift/close other tabs: the
        // index is only valid again through the id.
        const TabId id = tab->id();
        if (!fileLifecycle_->confirmCloseTab(*tab)) {
            return; // the user chose Cancel / Save (closes with the save)
        }
        index = workspace_.indexOfTab(id);
        if (index == DocumentWorkspace::kNoTab) return; // already closed
    }
    workspace_.closeTab(index);
}

void ShellController::setPresentationMode(bool enabled) {
    if (presentationMode_ == enabled) return;
    presentationMode_ = enabled;
    // layoutShell() hides the chrome (or restores its layout slots) and
    // positions everything else.
    viewport_->setPresentationMode(enabled);
    if (enabled) setFocus(nullptr);
    layoutShell();
}

void ShellController::setFocus(ui::Widget* widget) {
    if (focusedWidget_ == widget) return;
    if (focusedWidget_ != nullptr) focusedWidget_->setFocused(false);
    focusedWidget_ = widget;
    if (focusedWidget_ != nullptr) focusedWidget_->setFocused(true);
}

bool ShellController::handleKeyEvent(const ui::KeyEvent& event) {
    // 0. Escape cancels a print job being prepared before anything else.
    if (event.key == ui::Key::Escape && printCoordinator_.requestCancel()) return true;
    if (event.key == ui::Key::Escape && ocr_ != nullptr && ocr_->requestCancel()) return true;
    if (event.key == ui::Key::Escape && markdownExport_ != nullptr && markdownExport_->requestCancelPrint()) {
        return true;
    }

    // An open popover menu is modal; the toolbar's keyboard navigation next.
    if (popup_ != nullptr && popup_->isOpen()) return popup_->onKey(event);
    if (tooltip_ != nullptr) tooltip_->dismiss();
    if (toolbar_->keyboardNavigating()) {
        if (toolbar_->handleKey(event)) return true;
        toolbar_->endKeyboardNavigation();
    }

    // 1. The focused widget (a text field) consumes its keys first.
    if (focusedWidget_ != nullptr && focusedWidget_->onKey(event)) return true;

    // The signature sheet closes on Esc before anything underneath reacts.
    if (event.key == ui::Key::Escape && signatureSheet_ != nullptr && signatureSheet_->handleEscape()) return true;

    // Keys of the active viewport tool (Enter/Esc while cropping) run before
    // the shell's own Escape priorities.
    if (pageEditing_ != nullptr && pageEditing_->handleToolKey(event)) return true;

    // Escape: the content tools first (inline editor, a running gesture, the
    // selection), then the find bar / presentation mode below.
    if (event.key == ui::Key::Escape && content_ != nullptr && content_->handleEscape()) return true;
    // Then the Redact tool: a running drag, then the selected mark.
    if (event.key == ui::Key::Escape && redaction_ != nullptr && redaction_->handleEscape()) return true;
    // Then the form layer: its popup, then the focused field.
    if (event.key == ui::Key::Escape && forms_ != nullptr && forms_->handleEscape()) return true;

    // Escape closes the search bar / exits presentation mode; otherwise it
    // blurs the focused widget.
    if (event.key == ui::Key::Escape && searchBar_->handleEscape()) return true;
    if (event.key == ui::Key::Escape && presentationMode_) {
        setPresentationMode(false);
        return true;
    }
    if (event.key == ui::Key::Escape && focusedWidget_ != nullptr) {
        setFocus(nullptr);
        return true;
    }

    // 2. Application shortcuts (command/ctrl based).
    if (handleShortcut(event)) return true;

    // 3. Default: the active content (Markdown host or the PDF viewport).
    bool consumed = false;
    if (chromeMarkdown_) {
        consumed = markdownView_->onKey(event);
    } else if (chromeComparison_) {
        consumed = comparisonView_->onKey(event);
    } else {
        consumed = viewport_->onKey(event);
    }
    // 4. An unused Tab moves keyboard focus into the toolbar (ADR-0033 §5).
    if (!consumed && event.key == ui::Key::Tab && !event.modifiers.command && !event.modifiers.control &&
        !event.modifiers.option && focusedWidget_ == nullptr && !presentationMode_) {
        toolbar_->beginKeyboardNavigation(event.modifiers.shift);
        return toolbar_->keyboardNavigating();
    }
    return consumed;
}

bool ShellController::handleShortcut(const ui::KeyEvent& event) {
    // Markdown tabs: undo/redo go to the text command stack (rotate has no
    // meaning there) and Cmd+1/2/3 switch the display mode.
    if (context_->readyMarkdownTab() != nullptr) {
        const std::optional<PageEditCommand> edit = pageEditCommandForShortcut(event);
        if (edit == PageEditCommand::Undo || edit == PageEditCommand::Redo) {
            performPageEdit(*edit);
            return true;
        }
        if ((event.modifiers.command || event.modifiers.control) && !event.modifiers.shift &&
            !event.modifiers.option && event.key == ui::Key::Character) {
            if (event.text == "1") { performMarkdownMode(MarkdownDisplayMode::Rendered); return true; }
            if (event.text == "2") { performMarkdownMode(MarkdownDisplayMode::Source); return true; }
            if (event.text == "3") { performMarkdownMode(MarkdownDisplayMode::Split); return true; }
        }
    } else if (pageEditing_ != nullptr && pageEditing_->handleShortcut(event)) {
        // Page editing shortcuts (undo/redo, rotate).
        return true;
    }

    const bool command = event.modifiers.command;
    const bool control = event.modifiers.control;

    if (command || control) {
        switch (event.key) {
        case ui::Key::Plus:
            viewport_->zoomInStep();
            event.accepted = true;
            return true;
        case ui::Key::Minus:
            viewport_->zoomOutStep();
            event.accepted = true;
            return true;
        case ui::Key::Character:
            if (event.text == "0") {
                viewport_->zoomActualSize();
                return true;
            }
            if (event.text == "w") {
                requestCloseTab(workspace_.activeIndex());
                return true;
            }
            if (event.text == "s" && !event.modifiers.shift) {
                performFile(FileCommand::Save);
                return true;
            }
            if (event.text == "S" && event.modifiers.shift) {
                performFile(FileCommand::SaveAs);
                return true;
            }
            if (event.text == "f") {
                // Markdown: the bar drives the rendered text (Rendered mode) or the
                // source text (Source/Split).
                if (chromeComparison_) return true; // no find in comparison tabs
                if (!chromeMarkdown_ || markdownView_->searchTarget() != nullptr) searchBar_->toggle();
                return true;
            }
            if (event.text == "c") {
                if (chromeComparison_) return true; // no text selection in comparison tabs
                if (chromeMarkdown_) {
                    markdownView_->copySelection();
                } else {
                    textInteraction_->copySelection();
                }
                return true;
            }
            if (event.text == "a" && !event.modifiers.shift && chromeMarkdown_) {
                markdownView_->selectAll();
                return true;
            }
            if (event.text == "p" && !control) {
                handlePrintRequest();
                return true;
            }
            if (event.text == "p" && control && command && !chromeMarkdown_ && !chromeComparison_) {
                // Ctrl+Cmd+F is taken by macOS fullscreen; presentation uses
                // Ctrl+Cmd+P (print stays Cmd+P).
                setPresentationMode(!presentationMode_);
                return true;
            }
            if (event.text == "{" || event.text == "[") {
                activateAdjacentTab(-1);
                return true;
            }
            if (event.text == "}" || event.text == "]") {
                activateAdjacentTab(+1);
                return true;
            }
            break;
        case ui::Key::Tab:
            if (control) { // Ctrl+Tab cycles tabs
                activateAdjacentTab(+1);
                return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

void ShellController::activateAdjacentTab(int delta) {
    const std::size_t count = workspace_.tabCount();
    if (count == 0 || workspace_.activeIndex() == DocumentWorkspace::kNoTab) return;
    // Signed wraparound: prev from 0 lands on the last tab and vice versa.
    const long long next =
        (static_cast<long long>(workspace_.activeIndex()) + delta + static_cast<long long>(count)) %
        static_cast<long long>(count);
    workspace_.activateTab(static_cast<std::size_t>(next));
}

bool ShellController::isIdle() const {
    for (std::size_t i = 0; i < workspace_.tabCount(); ++i) {
        const DocumentTab* tab = workspace_.tab(i);
        if (tab != nullptr && tab->state() == DocumentTab::State::Loading) return false;
    }
    if (fileLifecycle_ != nullptr && fileLifecycle_->isSavingAnything()) return false;
    if (scheduler_.busyCount() != 0) return false;
    if (chromeMarkdown_ && markdownView_ != nullptr) {
        const MarkdownParseCoordinator* parser = markdownView_->parseCoordinator();
        if (parser != nullptr && parser->busy()) return false;
    }
    const bool pdfVisible = context_ != nullptr && context_->readyPdfTab() != nullptr && viewport_ != nullptr &&
                            !viewport_->frame().isEmpty();
    return !(pdfVisible && viewport_->lastPaintMissedTiles());
}

const std::string& ShellController::statusText() const {
    static const std::string kEmpty;
    return statusBar_ != nullptr ? statusBar_->statusText() : kEmpty;
}

std::string ShellController::searchCountText() const {
    if (searchBar_ == nullptr || !searchBar_->visible()) return {};
    return searchBar_->countText();
}

void ShellController::setStatus(std::string text) {
    if (statusBar_ != nullptr) statusBar_->setStatus(std::move(text));
}

void ShellController::setZoomDisplay(double zoom) {
    if (zoomControl_ != nullptr) zoomControl_->setPercentText(zoomPercentText(zoom));
}

void ShellController::updateWindowTitle() {
    if (services_.setWindowTitle == nullptr) return;
    const DocumentTab* tab = workspace_.activeTab();
    services_.setWindowTitle(tab != nullptr ? std::format("{} — Rivet", tab->title()) : "Rivet");
}

// Printing: the coordinator runs panel -> worker spool -> platform print for
// the ACTIVE tab; nothing is rasterized on the main thread.
void ShellController::handlePrintRequest() {
    if (DocumentTab* markdown = context_ != nullptr ? context_->readyMarkdownTab() : nullptr; markdown != nullptr) {
        if (markdownExport_ != nullptr) markdownExport_->print(*markdown);
        return;
    }
    DocumentTab* tab = readyActiveTab();
    if (tab == nullptr || tab->session() == nullptr) {
        setStatus("Nothing to print — open a document first");
        return;
    }
    // The printout reads the document as a whole: commit an open note
    // editor's text first.
    if (context_ != nullptr) context_->pendingEdits.commitAll();
    if (!confirmUnappliedRedactions(services_.alerts, *tab->session())) {
        setStatus("Print cancelled: redaction marks are not applied");
        return;
    }
    printCoordinator_.print(tab->id());
}

void ShellController::cancelPrintForTab(std::size_t index) {
    if (DocumentTab* tab = workspace_.tab(index); tab != nullptr && tab->session() != nullptr) {
        printCoordinator_.cancelIfDocument(tab->session()->id());
    }
}

std::unique_ptr<ShellController> createShell(const platform::ShellServices& services) {
    return ShellController::create(services);
}

} // namespace rivet::app
