#include "PdfiumLibrary.h"

#include "fpdfview.h"

namespace rivet::pdf {

PdfiumLibrary& PdfiumLibrary::instance() {
    static PdfiumLibrary library; // FPDF_InitLibraryWithConfig on first use
    return library;
}

PdfiumLibrary::PdfiumLibrary() {
    // Version 2 configuration. Zero-init leaves every optional feature
    // (external font paths, v8 isolate/slot, XFA, JavaScript) disabled, and
    // the explicit assignments below keep that intent visible at the call
    // site. Rivet never executes document JavaScript.
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    config.m_pUserFontPaths = nullptr;
    config.m_v8EmbedderSlot = 0;
    config.m_pIsolate = nullptr;
    config.m_pPlatform = nullptr;

    FPDF_InitLibraryWithConfig(&config);
    initialized_ = true;
}

PdfiumLibrary::~PdfiumLibrary() {
    if (initialized_) {
        FPDF_DestroyLibrary();
        initialized_ = false;
    }
}

} // namespace rivet::pdf
