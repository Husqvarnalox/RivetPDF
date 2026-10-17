// SPDX-License-Identifier: MPL-2.0
#pragma once

// Helpers shared by the PDFium writers (assembly, content edits, document
// builder). INTERNAL to src/pdf/pdfium/. Nothing here acquires the PDFium
// call gate: callers hold it.

#include "core/Error.hpp"
#include "pdf/PdfAssembly.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "fpdfview.h"

namespace rivet::pdf::internal {

// RAII wrapper for FPDF_DOCUMENT handles (load/create / FPDF_CloseDocument).
class ScopedDocument {
public:
    explicit ScopedDocument(FPDF_DOCUMENT document = nullptr) : document_(document) {}
    ~ScopedDocument() { reset(); }
    ScopedDocument(const ScopedDocument&) = delete;
    ScopedDocument& operator=(const ScopedDocument&) = delete;

    void reset() {
        if (document_ != nullptr) {
            FPDF_CloseDocument(document_);
            document_ = nullptr;
        }
    }
    FPDF_DOCUMENT get() const { return document_; }
    FPDF_DOCUMENT release() { return std::exchange(document_, nullptr); }

private:
    FPDF_DOCUMENT document_;
};

// Serializes `document` (FPDF_SaveAsCopy, FPDF_NO_INCREMENTAL: a complete,
// self-contained file) into `sink`. The first sink failure is recorded and
// every later block refused; a recorded sink error (or exception) is
// reported even when PDFium claims success, because PDFium drops the result
// of its final flush. Caller holds the gate.
//
// `removeSecurity` (ADR-0030): FPDF_REMOVE_SECURITY - the output carries no
// /Encrypt dictionary (objects are written decrypted), so it opens without a
// password. The caller must have opened the document with a valid password.
core::Status saveDocumentToSink(FPDF_DOCUMENT document, IPdfByteSink& sink, bool removeSecurity = false);

// In-memory FPDF_FILEACCESS source over a JPEG file (m_Param), for
// FPDFImageObj_LoadJpegFileInline. `data` must outlive the load call.
struct JpegSource {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};
int readJpegBlock(void* param, unsigned long position, unsigned char* buffer, unsigned long size);

} // namespace rivet::pdf::internal
