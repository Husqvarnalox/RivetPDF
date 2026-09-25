#pragma once

#include <mutex>
#include <utility>

namespace rivet::pdf {

// Process-wide serialization of every PDFium public API call.
//
// PDFium's public API is not thread-safe: no two FPDF_* calls may run
// concurrently in the process, even against different FPDF_DOCUMENTs. PDFium
// holds process-global state (glyph/image caches, FPDF_GetLastError), so
// unrelated calls race too. Per-document SerialExecutors (ADR-0006) order
// work within one document but run on different TaskScheduler workers and
// therefore do not prevent cross-document overlap; this gate does.
//
// Acquisition discipline (std::mutex is non-recursive; a nested invoke() is a
// guaranteed self-deadlock):
//   - Each public adapter entry operation acquires the gate exactly once,
//     covering its whole PDFium-touching body (e.g. PdfiumEngine::openDocument
//     wraps FPDF_LoadDocument + FPDF_GetLastError + PdfiumDocument
//     construction; ~PdfiumDocument wraps FPDF_CloseDocument).
//   - Internal helpers never acquire. Their contracts state "caller must hold
//     the PDFium gate" and they may only be reached from inside an invoke().
class PdfiumCallGate {
public:
    PdfiumCallGate(const PdfiumCallGate&) = delete;
    PdfiumCallGate& operator=(const PdfiumCallGate&) = delete;

    // Runs fn() while holding the process-wide PDFium lock and forwards its
    // result unchanged: value, reference, or void (decltype(auto) preserves
    // value categories). The lock is released before invoke() returns or
    // propagates an exception thrown by fn.
    template <typename F>
    decltype(auto) invoke(F&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::forward<F>(fn)();
    }

private:
    // Constructed only by globalPdfiumCallGate().
    PdfiumCallGate() = default;
    friend PdfiumCallGate& globalPdfiumCallGate();

    std::mutex mutex_;
};

// The process-wide gate. The instance is intentionally never destroyed
// (leaked): every PDFium call must be able to acquire it for the whole
// process lifetime, including document closes that happen during static
// teardown. A std::mutex needs no destructor work, so the leak costs
// nothing.
PdfiumCallGate& globalPdfiumCallGate();

} // namespace rivet::pdf
