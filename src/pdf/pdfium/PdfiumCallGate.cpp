#include "PdfiumCallGate.hpp"

namespace rivet::pdf {

PdfiumCallGate& globalPdfiumCallGate() {
    // Deliberate leak, never destroyed: see PdfiumCallGate.hpp. A function-
    // local static object would run its destructor during static teardown,
    // while document closes and FPDF_DestroyLibrary may still need the gate.
    static PdfiumCallGate* gate = new PdfiumCallGate;
    return *gate;
}

} // namespace rivet::pdf
