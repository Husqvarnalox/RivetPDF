// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "pdf/ImageReduce.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfContent.hpp"
#include "pdf/PdfForms.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
    // Annotation changes against the SOURCE page's /Annots (null or empty =
    // the page's annotations are copied unchanged). Applied after the view
    // and the content edits.
    std::shared_ptr<const PdfPageAnnotationEdits> annotationEdits;
    // Content changes against the SOURCE page's top-level objects (null or
    // empty = the page's content is copied untouched, byte for byte).
    // Applied after the view and before the annotation edits; never changes
    // the page's /Annots (PdfContent.hpp, ADR-0017). Declared last so the
    // positional aggregate initializers that predate it keep compiling.
    PdfPageContentEditsPtr contentEdits = nullptr;
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
    // Edited AcroForm values of `base`'s fields (ADR-0024; null or empty =
    // the form is copied unchanged). The backend applies them with its form
    // filler (JavaScript and XFA disabled) to the base pages reused in place
    // (PreserveBase) and to every copy of a base page it imports (duplicates;
    // Extract), so /V and the regenerated /AP are written. A field none of
    // whose widgets is part of the output is skipped. A field that cannot be
    // applied fails the assembly (never a silent partial save).
    PdfFormValuesPtr formValues;
    // Optional out-parameter: receives the names of fields that could not be
    // applied (the assembly then fails). Names are untrusted: never log them.
    std::vector<std::string>* formFailures = nullptr;
    // ADR-0026: write the document without its metadata (the /Info
    // dictionary and the catalog's XMP /Metadata). PDFium has no API to edit
    // either, so the backend rebuilds the output as a fresh document holding
    // the assembled pages: like Extract, document-level structure (outline,
    // AcroForm, page labels, structure tree, named destinations, ...) is not
    // carried over. Refused (InvalidArgument) for an encrypted base, whose
    // encryption a rebuild would drop.
    bool removeMetadata = false;
    // ADR-0028 (Save Reduced Copy): downsample and JPEG-re-encode the
    // top-level images of the PreserveBase output that exceed the target
    // resolution (see ImageReduce.hpp and the backend for the exact rules).
    // Ignored for Fresh assemblies.
    std::optional<PdfImageReduction> reduceImages;
    // ADR-0030 (Remove Password): write the document without its encryption
    // (PDFium FPDF_REMOVE_SECURITY). Only meaningful for PreserveBase of a
    // document opened with its password; refused (InvalidArgument) together
    // with removeMetadata.
    bool removeSecurity = false;
};

} // namespace rivet::pdf
