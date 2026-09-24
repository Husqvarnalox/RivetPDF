#pragma once

namespace rivet::pdf {

// Process-global PDFium library state.
//
// PDFium requires exactly one FPDF_InitLibraryWithConfig before any other
// FPDF_* call and one FPDF_DestroyLibrary at the end. This class owns that
// lifecycle: the first call to instance() initializes the library, and the
// singleton's destruction at process exit tears it down. Initialization is
// thread-safe (function-local static).
//
// The init configuration leaves every optional feature (external font paths,
// v8/XFA/JavaScript) disabled: Rivet never executes document JavaScript.
class PdfiumLibrary {
public:
    // Returns the process-wide instance, initializing PDFium on first use.
    static PdfiumLibrary& instance();

    PdfiumLibrary(const PdfiumLibrary&) = delete;
    PdfiumLibrary& operator=(const PdfiumLibrary&) = delete;

    // Whether FPDF_InitLibraryWithConfig has been called. With the Meyers
    // singleton above, a live instance is always initialized.
    bool isInitialized() const { return initialized_; }

private:
    PdfiumLibrary();
    ~PdfiumLibrary();

    bool initialized_ = false;
};

} // namespace rivet::pdf
