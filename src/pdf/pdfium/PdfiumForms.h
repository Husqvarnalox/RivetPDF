// SPDX-License-Identifier: MPL-2.0
#pragma once

// PDFium AcroForm adapter (ADR-0024): reading a document's fields into
// Rivet data and applying edited values with PDFium's form filler.
// INTERNAL to src/pdf/pdfium/. Every function here requires the caller to
// hold the PDFium gate and never acquires it.
//
// The form-fill environment is always created with FPDF_FORMFILLINFO
// version 1 (no XFA) and no JavaScript platform (m_pJsPlatform = nullptr);
// FORM_DoDocumentJSAction / OpenAction / AAction are never called. Field
// names and values are untrusted and never appear in error messages.

#include "core/Error.hpp"
#include "pdf/PdfForms.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "fpdf_formfill.h"
#include "fpdfview.h"

namespace rivet::pdf::internal {

// RAII form-fill environment over one document, plus the pages it has
// loaded. Created with FPDF_FORMFILLINFO version 1 (no XFA), no JavaScript
// platform and inert embedder callbacks (no timers, navigation, URI
// launches or named actions). PDFium asks the embedder for page handles
// (FFI_GetPage, e.g. to find the widget object of a check box), so the
// environment serves them: every page is loaded once with its form page
// view (FORM_OnAfterLoadPage) and closed (FORM_OnBeforeClosePage) by
// closePages() or before the environment exits. Caller holds the gate for
// the whole lifetime of the object (construction and destruction included).
class FormEnvironment {
public:
    explicit FormEnvironment(FPDF_DOCUMENT document);
    ~FormEnvironment();
    FormEnvironment(const FormEnvironment&) = delete;
    FormEnvironment& operator=(const FormEnvironment&) = delete;

    // Null when PDFium refused to create the environment.
    FPDF_FORMHANDLE get() const { return handle_; }
    // The page at `index`, loaded on first use (null when it cannot load).
    FPDF_PAGE page(int index);
    void closePages();

private:
    struct Info {
        FPDF_FORMFILLINFO base; // first: PDFium hands back &base
        FormEnvironment* owner = nullptr;
    };
    static FPDF_PAGE getPage(FPDF_FORMFILLINFO* info, FPDF_DOCUMENT document, int index);

    FPDF_DOCUMENT document_;
    Info info_;
    FPDF_FORMHANDLE handle_ = nullptr;
    std::map<int, FPDF_PAGE> pages_;
};

// Reads every widget of `reader` (pages 0..pageCount-1) and groups them into
// fields. `reader` should be a private, never-rendered document: creating
// the form environment may generate appearance streams (/NeedAppearances).
// A document without a form yields an empty info (no page is loaded).
core::Result<PdfFormInfo> readFormInfo(FPDF_DOCUMENT reader, std::size_t pageCount);

// Maps a SOURCE page index of the document `info` describes to the page of
// `document` that holds that page's widgets (nullopt = not in `document`).
using FormPageMap = std::function<std::optional<int>(std::size_t sourcePage)>;

// Applies `values` (validated against `info` first; InvalidArgument for an
// unknown field or an invalid value) to `document` through the form filler:
// text = focus + select all + replace selection; choice = set the selected
// indices; check box / radio = toggle the matching widget with the
// keyboard when its state differs; then focus is killed so PDFium writes /V
// and regenerates /AP. Fields none of whose widgets map into `document` are
// skipped. Every applied field is verified by reading it back; the names of
// fields that did not take their value go to `failures` (optional) and the
// call fails (InvalidDocument). /Annots is never changed.
core::Status applyFormValues(FPDF_DOCUMENT document,
                             const PdfFormInfo& info,
                             const PdfFormValues& values,
                             const FormPageMap& pageFor,
                             std::vector<std::string>* failures);

} // namespace rivet::pdf::internal
