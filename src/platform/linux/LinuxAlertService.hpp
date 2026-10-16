// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/Alerts.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace rivet::platform {

class X11Connection;
class X11Window;

// App-modal alerts for the X11 shell, drawn with Rivet's own widgets
// (ui::Button, ui::TextField) in a transient dialog window
// (_NET_WM_WINDOW_TYPE_DIALOG + _NET_WM_STATE_MODAL) over the owner.
//
// Modality mirrors -[NSAlert runModal]: the call runs a nested event loop
// (X11Connection::Mode::Modal) and returns the answer. During it only the
// dialog receives input; other windows still repaint and the clipboard is
// still served, but main-thread tasks (IMainThreadDispatcher::post) are NOT
// run until the dialog closes. Main thread only.
//
// Keys: Return = default button, Escape (or the window's close button) =
// Cancel, Ctrl+D = Don't Save (the macOS Cmd+D equivalent).
class LinuxAlertService final : public IAlertService {
public:
    LinuxAlertService(X11Connection& connection, X11Window* owner) : connection_(connection), owner_(owner) {}

    // The window dialogs are centered on / transient for (null = none).
    void setOwner(X11Window* owner) { owner_ = owner; }

    SaveChangesChoice askSaveChanges(std::string_view documentTitle) override;
    ReviewChangesChoice askReviewUnsavedChanges(std::size_t unsavedDocumentCount) override;
    void showError(std::string_view title, std::string_view message) override;
    std::optional<std::string> promptForText(std::string_view title, std::string_view message,
                                             std::string_view defaultText) override;

    // Two-button confirmation (Cancel | `confirmLabel`, the default). Used by
    // the Linux print and save dialogs and by IAlertService callers.
    bool confirm(std::string_view title, std::string_view message, std::string_view confirmLabel) override;

private:
    X11Connection& connection_;
    X11Window* owner_ = nullptr;
};

} // namespace rivet::platform
