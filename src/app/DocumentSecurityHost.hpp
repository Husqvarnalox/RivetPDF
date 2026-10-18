// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/DocumentWorkspace.hpp"
#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rivet::app {

// What the security controllers need to know about the active document.
struct SecurityDocumentInfo {
    enum class Signatures : std::uint8_t { Unknown, None, Present };

    bool available = false;  // a Ready PDF tab with a session
    TabId tab{};
    std::string title;
    std::filesystem::path path; // the file on disk, empty for an unsaved document
    std::size_t currentPage = 0; // 0-based, for the placement of a visible signature
    std::size_t pageCount = 0;
    bool dirty = false;
    bool encrypted = false;     // the document was opened with a password
    Signatures signatures = Signatures::Unknown; // Unknown: still loading
};

// The saved bytes of the document as PDFium would write them.
using PdfBytes = std::vector<std::uint8_t>;
// Runs on a worker. `cancelled` is polled between steps.
using PlainPdfProducer = std::function<core::Result<PdfBytes>(const std::function<bool()>& cancelled)>;

// The seam between the portable security controllers and the shell's
// document/session/dialog machinery. Production: ShellDocumentSecurityHost;
// tests use fakes. Main thread only.
class IDocumentSecurityHost {
public:
    virtual ~IDocumentSecurityHost() = default;

    virtual SecurityDocumentInfo activeDocument() = 0;
    // Native save panel; nullopt = cancelled.
    virtual std::optional<std::filesystem::path> chooseDestination(const std::string& suggestedName,
                                                                   const std::string& title) = 0;
    // Snapshot of the CURRENT in-memory document (edits included) written
    // through the existing save pipeline into a private temp file on the
    // worker and read back. Never touches the document's own file.
    virtual core::Result<PlainPdfProducer> captureSave(TabId tab) = 0;
    // Commits edits held by other controllers (open note editor) first.
    virtual void commitPendingEdits() {}
};

} // namespace rivet::app
