// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/Alerts.hpp"

#include <optional>
#include <string>
#include <string_view>

struct HWND__;

namespace rivet::platform {

class WindowsMainThreadDispatcher;

// App-modal alerts for the Win32 shell: TaskDialogIndirect (common controls
// v6, resolved at run time; MessageBoxW fallback) for the button prompts and
// a small in-memory dialog template (static text + edit + OK/Cancel) for
// promptForText. Modality mirrors -[NSAlert runModal]: the owner window is
// disabled, the call returns the answer, and main-thread tasks are deferred
// until the dialog closes (WindowsMainThreadDispatcher::ModalScope).
//
// Buttons follow Windows conventions: Save (default) / Don't Save (Alt+N) /
// Cancel (Esc); Review Changes... / Discard Changes / Cancel. Main thread only.
class WindowsAlertService final : public IAlertService {
public:
    WindowsAlertService(HWND__* owner, WindowsMainThreadDispatcher* dispatcher)
        : owner_(owner), dispatcher_(dispatcher) {}

    void setOwner(HWND__* owner) { owner_ = owner; }

    SaveChangesChoice askSaveChanges(std::string_view documentTitle) override;
    ReviewChangesChoice askReviewUnsavedChanges(std::size_t unsavedDocumentCount) override;
    void showError(std::string_view title, std::string_view message) override;
    std::optional<std::string> promptForText(std::string_view title, std::string_view message,
                                             std::string_view defaultText) override;

    // Two-button confirmation via TaskDialogIndirect: `okLabel` (default)
    // and Cancel (Esc). Also used by the save dialog's replace prompt.
    bool confirm(std::string_view title, std::string_view message, std::string_view okLabel) override;

private:
    HWND__* owner_ = nullptr;
    WindowsMainThreadDispatcher* dispatcher_ = nullptr;
};

} // namespace rivet::platform
