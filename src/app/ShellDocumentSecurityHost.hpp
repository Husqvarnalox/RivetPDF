// SPDX-License-Identifier: MPL-2.0
#pragma once

// Production IDocumentSecurityHost: answers from the active tab and captures
// the document through the existing save pipeline (editor::makeSaveJob +
// runDocumentWrite into a private temp file), never touching the open file.

#include "app/DocumentSecurityHost.hpp"
#include "app/ShellContext.hpp"

namespace rivet::pdf {
class PdfEngine;
}

namespace rivet::app {

class ShellDocumentSecurityHost final : public IDocumentSecurityHost {
public:
    ShellDocumentSecurityHost(pdf::PdfEngine& engine, ShellContext& context) : engine_(engine), context_(context) {}

    SecurityDocumentInfo activeDocument() override;
    std::optional<std::filesystem::path> chooseDestination(const std::string& suggestedName,
                                                           const std::string& title) override;
    core::Result<PlainPdfProducer> captureSave(TabId tab) override;
    void commitPendingEdits() override;

private:
    pdf::PdfEngine& engine_;
    ShellContext& context_;
};

} // namespace rivet::app
