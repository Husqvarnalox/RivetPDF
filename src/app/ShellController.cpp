#include "app/ShellController.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <format>

namespace rivet::app {

namespace {

constexpr double kToolbarHeight = 40.0;
constexpr double kStatusBarHeight = 26.0;
constexpr double kSidebarWidth = 220.0;

} // namespace

std::unique_ptr<ShellController> ShellController::create(const platform::ShellServices& services) {
    auto controller = std::unique_ptr<ShellController>(new ShellController(services));
    controller->buildWidgets();
    return controller;
}

ShellController::ShellController(const platform::ShellServices& services)
    : services_(services), scheduler_(0), engine_(pdf::createEngine()) {}

ShellController::~ShellController() {
    // The viewport must drop its render-source/layout pointers before the
    // session and the widget tree die.
    viewport_->clearDocument();
}

void ShellController::buildWidgets() {
    root_ = std::make_unique<ShellRoot>();
    root_->onLayout = [this] { layoutShell(); };

    auto toolbar = std::make_unique<ui::Toolbar>(kToolbarHeight);
    toolbar_ = toolbar.get();
    root_->addChild(std::move(toolbar));

    auto sidebar = std::make_unique<ui::Sidebar>();
    sidebar_ = sidebar.get();
    root_->addChild(std::move(sidebar));

    auto viewport = std::make_unique<ui::PdfViewport>();
    viewport_ = viewport.get();
    viewport_->setZoomChangedCallback([this](double zoom) { setZoomDisplay(zoom); });
    root_->addChild(std::move(viewport));

    auto statusBar = std::make_unique<ui::Container>();
    statusBar_ = statusBar.get();
    statusBar_->setBackgroundColor(ui::Color::rgba(0.93, 0.93, 0.93, 1.0));
    root_->addChild(std::move(statusBar));

    // Toolbar items. Fixed frames: the toolbar positions items from their
    // current frame sizes.
    auto openButton = std::make_unique<ui::Button>("Open");
    openButton->setOnClick([this] { handleOpenRequest(); });
    openButton->setFrame(core::Rect{0.0, 0.0, 64.0, 28.0});
    toolbar_->addItem(std::move(openButton), 12.0);

    auto zoomOut = std::make_unique<ui::Button>("−");
    zoomOut->setOnClick([this] { viewport_->zoom().zoomOut(); });
    zoomOut->setFrame(core::Rect{0.0, 0.0, 36.0, 28.0});
    toolbar_->addItem(std::move(zoomOut));

    auto zoomLabel = std::make_unique<TextLabel>("100%", ui::Font{13.0}, ui::Color::gray(0.25),
                                                 ui::TextAlign::Center);
    zoomLabel->setFrame(core::Rect{0.0, 0.0, 58.0, 28.0});
    zoomLabel_ = zoomLabel.get();
    toolbar_->addItem(std::move(zoomLabel));

    auto zoomIn = std::make_unique<ui::Button>("+");
    zoomIn->setOnClick([this] { viewport_->zoom().zoomIn(); });
    zoomIn->setFrame(core::Rect{0.0, 0.0, 36.0, 28.0});
    toolbar_->addItem(std::move(zoomIn));

    auto actualSize = std::make_unique<ui::Button>("1:1");
    actualSize->setOnClick([this] { viewport_->zoom().actualSize(); });
    actualSize->setFrame(core::Rect{0.0, 0.0, 52.0, 28.0});
    toolbar_->addItem(std::move(actualSize), 0.0);

    auto statusLabel = std::make_unique<TextLabel>(
        engine_->isAvailable() ? "Ready" : "Ready — PDF support is not built into this binary",
        ui::Font{12.0}, ui::Color::gray(0.35), ui::TextAlign::Left);
    statusLabel->setFrame(core::Rect{0.0, 0.0, 100.0, kStatusBarHeight});
    statusLabel_ = statusLabel.get();
    statusBar_->addChild(std::move(statusLabel));

    updateSidebar();
    setZoomDisplay(viewport_->zoom().zoom());
}

void ShellController::layoutShell() {
    const core::Rect bounds = root_->bounds();
    const double width = bounds.size.width;
    const double height = bounds.size.height;

    const double middleHeight = std::max(0.0, height - kToolbarHeight - kStatusBarHeight);
    toolbar_->setFrame(core::Rect{0.0, 0.0, width, kToolbarHeight});
    sidebar_->setFrame(core::Rect{0.0, kToolbarHeight, kSidebarWidth, middleHeight});
    viewport_->setFrame(core::Rect{kSidebarWidth, kToolbarHeight,
                                   std::max(0.0, width - kSidebarWidth), middleHeight});
    statusBar_->setFrame(core::Rect{0.0, height - kStatusBarHeight, width, kStatusBarHeight});
    statusLabel_->setFrame(core::Rect{0.0, 0.0, std::max(0.0, width - 24.0), kStatusBarHeight});
}

void ShellController::handleOpenRequest() {
    if (services_.fileDialog == nullptr) {
        setStatus("No file dialog available on this platform backend");
        return;
    }
    const core::Result<std::filesystem::path> chosen = services_.fileDialog->openPdf();
    if (!chosen.has_value()) {
        if (chosen.error().code != core::ErrorCode::Cancelled) {
            setStatus("Could not choose a file: " + core::describe(chosen.error()));
        }
        return;
    }
    openDocument(*chosen);
}

void ShellController::openDocument(const std::filesystem::path& path) {
    if (!engine_->isAvailable()) {
        setStatus("PDF support is not built into this binary (RIVET_WITH_PDFIUM=OFF)");
        return;
    }

    closeDocument();

    core::Result<std::unique_ptr<editor::DocumentSession>> opened =
        editor::DocumentSession::create(*engine_, scheduler_, services_.mainDispatcher, path);
    if (!opened.has_value()) {
        setStatus("Failed to open document: " + core::describe(opened.error()));
        core::log::warning("openDocument failed: " + core::describe(opened.error()));
        return;
    }

    session_ = std::move(*opened);
    editor::DocumentSession* session = session_.get();
    viewport_->setDocument(session->id(), &session->layout(), &session->renderSource(),
                           [session] { return session->revision(); });

    // Fit the view to the window width; the fit intent is resolved by the
    // next layout pass on the viewport.
    viewport_->zoom().setFitMode(render::ZoomState::FitMode::Width);
    viewport_->layout();

    updateSidebar();
    setStatus(std::format("Opened {} — {} page{}", path.filename().string(), session->pageCount(),
                          session->pageCount() == 1 ? "" : "s"));
    core::log::info(std::format("opened document with {} pages", session->pageCount()));
    viewport_->invalidate();
}

void ShellController::closeDocument() {
    viewport_->clearDocument();
    session_.reset();
    updateSidebar();
    setZoomDisplay(viewport_->zoom().zoom());
}

void ShellController::updateSidebar() {
    if (session_ == nullptr) {
        sidebar_->setItems({"No document open"});
        sidebar_->setSelectedIndex(std::nullopt);
        return;
    }

    std::vector<std::string> items;
    items.reserve(session_->pageCount());
    for (std::size_t i = 0; i < session_->pageCount(); ++i) {
        const core::Size size = session_->pageSizePoints(i);
        items.push_back(std::format("Page {}  ({:.0f} x {:.0f})", i + 1, size.width, size.height));
    }
    sidebar_->setItems(std::move(items));
    sidebar_->setSelectedIndex(std::nullopt);

    sidebar_->setOnSelectionChanged([this](std::size_t index) {
        if (session_ == nullptr || index >= session_->pageCount()) return;
        const double y = session_->layout().pageTopOffsetPoints(index);
        viewport_->setScrollOffsetPoints(core::Point{0.0, y});
        viewport_->invalidate();
    });
}

void ShellController::setStatus(std::string text) {
    if (statusLabel_ != nullptr) statusLabel_->setText(std::move(text));
}

void ShellController::setZoomDisplay(double zoom) {
    if (zoomLabel_ != nullptr) zoomLabel_->setText(std::format("{:.0f}%", zoom * 100.0));
}

std::unique_ptr<ShellController> createShell(const platform::ShellServices& services) {
    return ShellController::create(services);
}

} // namespace rivet::app
