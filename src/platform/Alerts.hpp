// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rivet::platform {

// Answer to the single-document unsaved-changes prompt.
enum class SaveChangesChoice : std::uint8_t { Save, DontSave, Cancel };

// Answer to the multi-document prompt shown when quitting with several
// unsaved documents: review each one, discard all, or keep running.
enum class ReviewChangesChoice : std::uint8_t { Review, DiscardAll, Cancel };

// Native modal alerts. Implemented per platform (NSAlert on macOS). All
// calls block until answered (app-modal) and must be made on the main
// thread. Strings are UTF-8.
class IAlertService {
public:
    virtual ~IAlertService() = default;

    // "Do you want to save the changes made to the document “<title>”?"
    // Buttons: Save (default), Don't Save (Cmd+D), Cancel (Esc).
    virtual SaveChangesChoice askSaveChanges(std::string_view documentTitle) = 0;

    // "You have <count> documents with unsaved changes. Do you want to
    // review these changes before quitting?"
    // Buttons: Review Changes… (default), Discard Changes, Cancel (Esc).
    virtual ReviewChangesChoice askReviewUnsavedChanges(std::size_t unsavedDocumentCount) = 0;

    // Warning alert with a single OK button.
    virtual void showError(std::string_view title, std::string_view message) = 0;

    // Modal single-line text prompt (OK / Cancel) pre-filled with
    // `defaultText`. Returns the entered text, or nullopt when cancelled or
    // when the platform has no text prompt (the default).
    virtual std::optional<std::string> promptForText(std::string_view title, std::string_view message,
                                                     std::string_view defaultText) {
        (void)title;
        (void)message;
        (void)defaultText;
        return std::nullopt;
    }
};

} // namespace rivet::platform
