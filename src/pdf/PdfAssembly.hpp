// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rivet::pdf {

class PdfDocument;

// Destination of the bytes a document assembly produces (e.g. an in-memory
// buffer or a temporary file that is atomically renamed into place once the
// assembly succeeded). write() receives the serialized PDF in order; a
// non-ok Status aborts the assembly, which then reports that error.
//
// Backends may call write() while holding engine-internal locks: an
// implementation must not call back into any PdfEngine/PdfDocument method
// (see PdfEngine::assembleDocument).
class IPdfByteSink {
public:
    virtual ~IPdfByteSink() = default;
    virtual core::Status write(const void* data, std::size_t size) = 0;
};

// One page of an assembled document: page `sourcePageIndex` of `source`,
// presented through `view` (absolute rotation + crop box in the source
// page's user space; the crop box must lie within the page's media box).
struct PdfAssemblyPage {
    const PdfDocument* source = nullptr;
    std::size_t sourcePageIndex = 0;
    PdfPageView view;
};

// A request to materialize a page list into a new PDF.
//
// PreserveBase (Save): document-level structure - outline, metadata, forms,
// viewer preferences, the page-label tree, encryption - comes from `base`,
// whose pages are reused in place where possible.
//
// Fresh (Extract): a brand-new document holding only the listed pages; no
// document-level structure is carried over (see the backend for details).
//
// `pages` is the final page order and must be non-empty. All document
// pointers must be documents opened by the engine the request is sent to.
struct PdfAssemblyRequest {
    enum class Mode : std::uint8_t { PreserveBase, Fresh };
    Mode mode = Mode::PreserveBase;
    const PdfDocument* base = nullptr;
    std::vector<PdfAssemblyPage> pages;
};

} // namespace rivet::pdf
