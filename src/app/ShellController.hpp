#pragma once

#include "app/TextLabel.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSession.hpp"
#include "pdf/PdfSystem.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/Sidebar.hpp"
#include "ui/Toolbar.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace rivet::app {

// The Rivet shell: owns the widget tree (toolbar / sidebar / viewport / status
// bar), the shared task scheduler, the PDF engine and the current document
// session. Main-thread only.
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

    // Keyboard entry point: the host forwards key events here (the viewport
    // is the only keyboard consumer in this shell).
    bool handleKeyEvent(const ui::KeyEvent& event) { return viewport_->onKey(event); }

    // Triggered by the Open button; shows the platform dialog and opens the
    // chosen document.
    void handleOpenRequest();

    // Opens a document directly (no dialog). Used by handleOpenRequest() and
    // by the platform entry point for open-on-launch (`rivet file.pdf`).
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
    void layoutShell();
    void closeDocument();
    void updateSidebar();
    void setStatus(std::string text);
    void setZoomDisplay(double zoom);

    platform::ShellServices services_;
    core::TaskScheduler scheduler_;
    std::unique_ptr<pdf::PdfEngine> engine_;
    std::unique_ptr<editor::DocumentSession> session_;

    // Widget tree. The shell widgets are created in buildWidgets(); root_
    // owns the whole tree afterwards.
    std::unique_ptr<ShellRoot> root_;
    ui::Toolbar* toolbar_ = nullptr;
    ui::Sidebar* sidebar_ = nullptr;
    ui::PdfViewport* viewport_ = nullptr;
    ui::Container* statusBar_ = nullptr;

    // Raw pointers into widgets owned by the containers above.
    TextLabel* zoomLabel_ = nullptr;
    TextLabel* statusLabel_ = nullptr;
};

// Factory used by the platform entry point (main).
std::unique_ptr<ShellController> createShell(const platform::ShellServices& services);

} // namespace rivet::app
