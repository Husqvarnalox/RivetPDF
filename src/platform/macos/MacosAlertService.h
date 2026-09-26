// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/Alerts.hpp"

namespace rivet::platform {

// NSAlert-backed modal alerts, run app-modal (runModal). Main thread only.
class MacosAlertService final : public IAlertService {
public:
    SaveChangesChoice askSaveChanges(std::string_view documentTitle) override;
    ReviewChangesChoice askReviewUnsavedChanges(std::size_t unsavedDocumentCount) override;
    void showError(std::string_view title, std::string_view message) override;
};

} // namespace rivet::platform
