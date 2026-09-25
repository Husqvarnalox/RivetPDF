#include "PdfiumLibrary.h"

#include "PdfiumCallGate.hpp"

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
    globalPdfiumCallGate().invoke([&] {
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        config.m_pUserFontPaths = nullptr;
        config.m_v8EmbedderSlot = 0;
        config.m_pIsolate = nullptr;
        config.m_pPlatform = nullptr;

        FPDF_InitLibraryWithConfig(&config);
    });
    initialized_ = true;
}

PdfiumLibrary::~PdfiumLibrary() {
    if (initialized_) {
        // The PDFium call gate is a leaked singleton, so it is still
        // acquirable here during static teardown. The gate only serializes;
        // it cannot make calls safe after FPDF_DestroyLibrary. Ownership
        // guarantees no FPDF_DOCUMENT outlives this destructor: sessions die
        // before main returns, while this static outlives them.
        globalPdfiumCallGate().invoke([] { FPDF_DestroyLibrary(); });
        initialized_ = false;
    }
}

} // namespace rivet::pdf
