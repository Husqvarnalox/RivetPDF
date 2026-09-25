#pragma once

#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"

#include <filesystem>

namespace rivet::ui {
struct IRedrawSink;
}

namespace rivet::platform {

// File-dialog abstraction. Implemented per platform (NSOpenPanel on macOS).
// Shared application code must use this interface, never AppKit.
class IFileDialog {
public:
    virtual ~IFileDialog() = default;

    // Shows a modal open dialog filtered to PDF files.
    // ErrorCode::Cancelled means the user dismissed the dialog.
    virtual core::Result<std::filesystem::path> openPdf() = 0;
};

// Services the platform window shell hands to the application shell.
// All pointers are owned by the platform host and must outlive the app shell.
struct ShellServices {
    // Repaint request sink for the widget tree (requests a frame on the
    // platform window). Thread-safe: render completion callbacks invoke it
    // from worker threads via invalidate().
    ui::IRedrawSink* redrawSink = nullptr;

    // Marshals work to the platform main/UI thread (render results, tile
    // repaints). Required in production; render callbacks are delivered
    // through it.
    core::IMainThreadDispatcher* mainDispatcher = nullptr;

    // Native open-file dialog. May be null (dialog unavailable).
    IFileDialog* fileDialog = nullptr;

    // Sets the window title (UTF-8). May be null; the shell then never
    // touches the platform window title. Used for the active document name.
    std::function<void(const std::string&)> setWindowTitle;
};

} // namespace rivet::platform
