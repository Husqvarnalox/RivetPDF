// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "platform/Alerts.hpp"
#include "platform/AppLifecycle.hpp"
#include "platform/Clipboard.hpp"
#include "platform/ExternalUrlOpener.hpp"
#include "platform/Print.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

    struct OpenOptions {
        std::string title = "Open PDF";
        std::string prompt = "Open";
        bool allowMultiple = false; // e.g. "Insert Pages from File…"
    };

    // Open dialog returning one or more PDFs (non-empty on success).
    // ErrorCode::Cancelled means the user dismissed the dialog. The default
    // falls back to the single-selection openPdf().
    virtual core::Result<std::vector<std::filesystem::path>> openPdfs(const OpenOptions& options) {
        (void)options;
        auto one = openPdf();
        if (!one) return std::unexpected(one.error());
        return std::vector<std::filesystem::path>{std::move(*one)};
    }
};

// Save-as dialog abstraction (NSSavePanel on macOS). App-modal; main thread
// only.
class ISaveDialog {
public:
    struct Options {
        std::string suggestedName;              // e.g. "Report.pdf"
        std::filesystem::path initialDirectory; // empty = platform default
        std::string title = "Save";
        std::string prompt = "Save";
    };

    virtual ~ISaveDialog() = default;

    // Returns the chosen path (with a .pdf extension), or nullopt when the
    // user cancelled. The native panel has already asked the user to
    // confirm replacing an existing file, so the caller may overwrite it.
    virtual std::optional<std::filesystem::path> runSavePanel(const Options& options) = 0;
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

    // System clipboard. May be null (copy disabled on this backend).
    IClipboard* clipboard = nullptr;

    // System URL opener. May be null (external links then do nothing).
    IExternalUrlOpener* urlOpener = nullptr;

    // Native print service. May be null (printing disabled).
    IPrintService* printService = nullptr;

    // Sets the window title (UTF-8). May be null; the shell then never
    // touches the platform window title. Used for the active document name.
    std::function<void(const std::string&)> setWindowTitle;

    // Native save-as dialog. May be null (Save As unavailable).
    ISaveDialog* saveDialog = nullptr;

    // Native modal alerts (unsaved-changes prompts, error reports). May be
    // null; callers then fall back to in-window status messages and must
    // treat an unanswerable unsaved-changes prompt as Cancel (never discard
    // data silently).
    IAlertService* alerts = nullptr;

    // Window-close / quit interception. May be null (e.g. tests: nothing to
    // intercept). The app layer registers handlers and must reset them to
    // empty before the objects they capture die.
    IAppLifecycle* lifecycle = nullptr;

    // Marks the platform window as having unsaved changes (macOS: the dot in
    // the close button, -[NSWindow setDocumentEdited:]). May be null. Main
    // thread.
    std::function<void(bool)> setDocumentEdited;
};

} // namespace rivet::platform
