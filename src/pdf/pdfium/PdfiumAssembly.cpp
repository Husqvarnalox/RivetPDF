// SPDX-License-Identifier: MPL-2.0

#include "PdfiumAssembly.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "PdfiumCallGate.hpp"
#include "PdfiumDisplayTransform.h"
#include "PdfiumDocument.h"
#include "PdfiumEngine.h"
#include "PdfiumFileSource.h"
#include "fpdf_edit.h"          // FPDF_CreateNewDocument, FPDF_MovePages, FPDFPage_Delete, FPDFPage_SetRotation
#include "fpdf_ppo.h"           // FPDF_ImportPagesByIndex
#include "fpdf_save.h"          // FPDF_SaveAsCopy
#include "fpdf_transformpage.h" // FPDFPage_SetCropBox
#include "fpdfview.h"

// Document assembly: materializes a Rivet page list into a new PDF.
//
// Ground rule: the LIVE documents (the ones the editor is presenting) are
// never mutated. PDFium's page-tree edits are not transactional -
// FPDF_MovePages documents that on failure "the document may be left in an
// indeterminate state" (CPDF_Document::MovePages deletes all moved pages
// first and re-inserts them one by one), and FPDF_ImportPagesByIndex inserts
// each page before copying it, so a failure midway leaves half-imported
// pages behind. All edits therefore happen on a private working
// FPDF_DOCUMENT that is closed (RAII) whatever happens; the live documents
// are only READ (FPDF_LoadPage for validation, and as import sources).
//
// PreserveBase (Save):
//   1. working = FPDF_LoadCustomDocument over the base's shared file source
//      and stored password: the exact bytes the live document was opened
//      from, even if the path was replaced meanwhile.
//   2. The FIRST occurrence of each base page (source == base) is reused in
//      place: its page object, and everything that points at it (outline
//      and link destinations, form fields, structure tree), survives.
//   3. Every other page - duplicates of base pages and pages of other
//      documents - is appended with FPDF_ImportPagesByIndex, ONE call per
//      source document (in order of first appearance; a superset of
//      batching consecutive runs), so resources shared between the pages of
//      one source (fonts, images) are copied once. Duplicates of base pages
//      are imported from the live base document (read-only).
//   4. One FPDF_MovePages(working, finalIndices, M, 0) brings the final
//      order to the front (skipped when it already is), then FPDFPage_Delete
//      removes the leftovers from the end backwards.
//   5. Each final page whose effective /Rotate or crop box differs from its
//      requested view gets FPDFPage_SetRotation/FPDFPage_SetCropBox; the
//      result is re-read and verified.
//   6. FPDF_SaveAsCopy(working, FPDF_NO_INCREMENTAL) streams into the sink.
//
// Fresh (Extract): working = FPDF_CreateNewDocument(); pages are imported in
// final order (one FPDF_ImportPagesByIndex per run of consecutive pages
// from the same source), views applied, saved. NOT carried over: the
// outline, document metadata (/Info title/author/...; PDFium writes its own
// /Producer "PDFium" and a /CreationDate), XMP metadata, AcroForm (imported
// widget annotations lose their fields), the page-label tree, the structure
// tree (tagging), named destinations, viewer preferences, attachments,
// JavaScript, encryption, and link destinations pointing at pages outside
// the imported set (PDFium drops such /Dest entries while importing).
//
// PDFium behaviors this relies on / limitations (verified against the
// source of the pinned revision):
//   - Encryption (PreserveBase): CPDF_Creator takes the parser's /Encrypt
//     dictionary and security handler, re-encrypts every written object with
//     it and keeps the first /ID element (the key-derivation input), so a
//     non-incremental save of a document opened with a password stays
//     encrypted with the SAME passwords and permissions. Only the second
//     /ID element is regenerated (random). If the original had no /ID, an
//     R2/R3 Standard handler is re-derived from the stored password for the
//     new random ID (CPDF_Creator::InitID).
//   - Unreferenced objects / privacy: a non-incremental save writes an
//     ORIGINAL (parsed) object only if it is reachable from the trailer
//     (CPDF_Creator::WriteOldObjs filters by GetObjectsWithReferences), so a
//     deleted page's content streams, images and fonts are dropped - UNLESS
//     another reachable structure still references them: form fields in
//     /AcroForm /Fields whose widgets sit on the deleted page (plus their
//     appearance streams), structure-tree elements (/StructTreeRoot /Pg and
//     marked content), article threads (/B beads), or other pages sharing
//     the resources. Deleted page dictionaries themselves are replaced by
//     `null` (CPDF_Document::SetPageToNullObject), so outline items/links
//     that targeted a deleted page point to a null object (dangling
//     destination; viewers show them as no-ops). NEW objects (created by
//     imports, never in the original file) are ALL written regardless of
//     reachability (CPDF_Creator::WriteNewObjs) - which is why this
//     assembly never imports a page it then deletes.
//   - Imports: FPDF_ImportPagesByIndex copies the page dictionary, pulls in
//     inheritable MediaBox/Resources/CropBox/Rotate, and deep-copies every
//     referenced object except other pages: references to pages that are
//     not part of the same import (link /Dest arrays, etc.) are removed.
//     It also sets /Producer "PDFium" in the destination's /Info if that
//     dictionary exists - i.e. a PreserveBase save that imports pages
//     changes the base's /Producer entry (no public API to restore it).
//   - Page moves re-parent pages: CPDF_Document::MovePages removes and
//     re-inserts page references, possibly under a different intermediate
//     /Pages node. Attributes inherited from intermediate nodes (/Rotate,
//     /CropBox, /MediaBox, /Resources) can change for a moved page in a
//     multi-level page tree. Rotation and crop box are re-verified per page
//     below (a mismatch is fixed or reported); an inherited /Resources or
//     /MediaBox change cannot be repaired through the public API.
//   - Page labels (/PageLabels) are keyed by page INDEX, so after a reorder
//     or delete the labels stay attached to positions, not to pages; the
//     outline and link destinations are keyed by page OBJECT and follow the
//     pages (or dangle when the page was deleted).
//   - Output is not byte-deterministic: CPDF_Creator generates a random
//     second /ID element (and a random first one for documents without an
//     /ID, e.g. every Fresh document).
//   - FPDF_SaveAsCopy's final chunk is flushed from a destructor whose
//     result is ignored (FileBufferArchive::~FileBufferArchive), so a sink
//     failure on the LAST write does not make FPDF_SaveAsCopy return false.
//     The sink adapter records failures itself and the assembly fails on
//     any recorded sink error.
//
// Gate strategy: the whole assembly runs inside ONE acquisition of the
// global PDFium call gate. The alternative (re-acquiring per step) would let
// renders interleave, but buys little: the dominant cost is
// FPDF_SaveAsCopy, which is one indivisible call anyway, and the working
// document's intermediate states are nobody else's business. One
// acquisition also keeps the adapter's "one acquisition per public entry
// operation" rule without exceptions. Consequences: rendering of every
// document stalls while a save runs, and the sink runs under the gate (it
// must not call back into the PDF layer, or it self-deadlocks).
//
// Error model: every failure returns an Error (InvalidArgument for bad
// requests, the sink's own error for sink failures, InvalidDocument/Internal
// for PDFium failures); exceptions never cross this boundary (bad_alloc maps
// to OutOfMemory), and all PDFium handles are RAII-closed on every path.

namespace rivet::pdf {
namespace {

// RAII wrapper for FPDF_DOCUMENT handles (load/create / FPDF_CloseDocument).
class ScopedDocument {
public:
    explicit ScopedDocument(FPDF_DOCUMENT document) : document_(document) {}
    ~ScopedDocument() {
        if (document_ != nullptr) {
            FPDF_CloseDocument(document_);
        }
    }
    ScopedDocument(const ScopedDocument&) = delete;
    ScopedDocument& operator=(const ScopedDocument&) = delete;

    FPDF_DOCUMENT get() const { return document_; }

private:
    FPDF_DOCUMENT document_;
};

// Page counts and indices cross the PDFium API as int.
constexpr std::size_t kMaxPages = static_cast<std::size_t>(std::numeric_limits<int>::max() / 2);

core::Error invalidArgument(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "pdf");
}

core::Error internalError(std::string message) {
    return core::makeError(core::ErrorCode::Internal, std::move(message), "pdf");
}

// One requested page, resolved to a document of this engine.
struct ResolvedPage {
    const PdfiumDocument* source = nullptr;
    int sourceIndex = 0;
    PdfPageView view;
};

// Resolves a request's document pointer: it must be a live PdfiumDocument
// opened by `engine`.
core::Result<const PdfiumDocument*> ownedDocument(const PdfiumEngine& engine,
                                                  const PdfDocument* document,
                                                  const char* role) {
    if (document == nullptr) {
        return std::unexpected(invalidArgument(std::string(role) + " document is null"));
    }
    const auto* pdfium = dynamic_cast<const PdfiumDocument*>(document);
    if (pdfium == nullptr || pdfium->owner() != &engine || pdfium->handle() == nullptr ||
        pdfium->fileSource() == nullptr) {
        return std::unexpected(
            invalidArgument(std::string(role) + " document was not opened by this PDF engine"));
    }
    return pdfium;
}

// Validates the request and resolves every page. Caller holds the gate:
// view validation loads each distinct source page once (read-only, like a
// render) to learn its native view and media box.
core::Result<std::vector<ResolvedPage>> resolvePages(const PdfiumEngine& engine,
                                                     const PdfAssemblyRequest& request) {
    if (request.pages.empty()) {
        return std::unexpected(invalidArgument("an assembled document needs at least one page"));
    }
    if (request.pages.size() > kMaxPages) {
        return std::unexpected(invalidArgument("too many pages in the assembly request"));
    }

    struct PageFacts {
        PdfPageView nativeView;
        PdfBox mediaBox;
    };
    std::map<std::pair<const PdfiumDocument*, int>, PageFacts> facts;

    std::vector<ResolvedPage> resolved;
    resolved.reserve(request.pages.size());
    for (std::size_t i = 0; i < request.pages.size(); ++i) {
        const PdfAssemblyPage& page = request.pages[i];
        const auto source = ownedDocument(engine, page.source, "page source");
        if (!source.has_value()) {
            return std::unexpected(source.error());
        }
        const std::size_t count = (*source)->info().pageCount;
        if (page.sourcePageIndex >= count) {
            return std::unexpected(invalidArgument(
                "assembly page " + std::to_string(i) + ": source page index " +
                std::to_string(page.sourcePageIndex) + " out of range (document has " +
                std::to_string(count) + " pages)"));
        }
        const int sourceIndex = static_cast<int>(page.sourcePageIndex);

        const auto key = std::make_pair(*source, sourceIndex);
        auto known = facts.find(key);
        if (known == facts.end()) {
            internal::ScopedPage loaded(FPDF_LoadPage((*source)->handle(), sourceIndex));
            if (loaded.get() == nullptr) {
                const int lastError = static_cast<int>(FPDF_GetLastError());
                return std::unexpected(core::makeError(
                    core::ErrorCode::InvalidDocument,
                    "PDFium failed to load source page " + std::to_string(sourceIndex) +
                        " (FPDF error " + std::to_string(lastError) + ")",
                    "pdf"));
            }
            const std::optional<PdfPageView> native = internal::nativePageView(loaded.get());
            if (!native.has_value()) {
                return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                                       "source page " + std::to_string(sourceIndex) +
                                                           " has invalid display dimensions",
                                                       "pdf"));
            }
            known = facts.emplace(key, PageFacts{*native, internal::pageMediaBox(loaded.get(), *native)})
                        .first;
        }
        const core::Status valid = internal::checkPageView(page.view, known->second.nativeView,
                                                           known->second.mediaBox, page.sourcePageIndex);
        if (!valid.has_value()) {
            return std::unexpected(invalidArgument("assembly page " + std::to_string(i) + ": " +
                                                   valid.error().message));
        }
        resolved.push_back(ResolvedPage{*source, sourceIndex, page.view});
    }
    return resolved;
}

// Imports `indices` of `source` into `working` at `insertAt`. Caller holds
// the gate.
core::Status importPages(FPDF_DOCUMENT working,
                         const PdfiumDocument& source,
                         const std::vector<int>& indices,
                         int insertAt) {
    if (FPDF_ImportPagesByIndex(working, source.handle(), indices.data(),
                                static_cast<unsigned long>(indices.size()), insertAt) == 0) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               "PDFium failed to import " + std::to_string(indices.size()) +
                                                   " page(s) into the working document",
                                               "pdf"));
    }
    return core::ok();
}

bool sameBox(const PdfBox& a, const PdfBox& b) {
    return a.contains(b, internal::kBoxEpsilon) && b.contains(a, internal::kBoxEpsilon);
}

// Makes page i of `working` present pages[i].view: /Rotate and /CropBox are
// written only where the effective values differ, then re-read and
// verified. Caller holds the gate.
core::Status applyViews(FPDF_DOCUMENT working, const std::vector<ResolvedPage>& pages) {
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const PdfPageView& wanted = pages[i].view;
        internal::ScopedPage page(FPDF_LoadPage(working, static_cast<int>(i)));
        if (page.get() == nullptr) {
            const int lastError = static_cast<int>(FPDF_GetLastError());
            return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                                   "PDFium failed to load assembled page " +
                                                       std::to_string(i) + " (FPDF error " +
                                                       std::to_string(lastError) + ")",
                                                   "pdf"));
        }

        const std::optional<PdfPageView> current = internal::nativePageView(page.get());
        if (!current.has_value() || current->rotation != wanted.rotation) {
            FPDFPage_SetRotation(page.get(), static_cast<int>(wanted.rotation));
        }
        if (!current.has_value() || !sameBox(current->cropBox, wanted.cropBox)) {
            FPDFPage_SetCropBox(page.get(), static_cast<float>(wanted.cropBox.left),
                                static_cast<float>(wanted.cropBox.bottom),
                                static_cast<float>(wanted.cropBox.right),
                                static_cast<float>(wanted.cropBox.top));
        }

        // Verify: the effective crop box is CropBox ∩ MediaBox, so a media
        // box that changed underneath (inheritance after a move) shows here.
        const std::optional<PdfPageView> applied = internal::nativePageView(page.get());
        if (!applied.has_value() || applied->rotation != wanted.rotation ||
            !sameBox(applied->cropBox, wanted.cropBox)) {
            return std::unexpected(internalError("assembled page " + std::to_string(i) +
                                                 " does not present the requested view"));
        }
    }
    return core::ok();
}

// FPDF_FILEWRITE adapter forwarding to an IPdfByteSink. The first failure
// is recorded and every later block is refused, so nothing after a failed
// block ever reaches the sink.
struct SinkWriter final : FPDF_FILEWRITE {
    IPdfByteSink* sink = nullptr;
    std::optional<core::Error> error;
    bool threw = false;
    std::uint64_t bytesWritten = 0;
};

// Called from inside PDFium: never lets an exception escape.
int writeToSink(FPDF_FILEWRITE* self, const void* data, unsigned long size) noexcept {
    auto* writer = static_cast<SinkWriter*>(self);
    if (writer->error.has_value() || writer->threw) {
        return 0;
    }
    if (size == 0) {
        return 1;
    }
    try {
        core::Status status = writer->sink->write(data, static_cast<std::size_t>(size));
        if (!status.has_value()) {
            writer->error.emplace(std::move(status).error());
            return 0;
        }
    } catch (...) {
        writer->threw = true;
        return 0;
    }
    writer->bytesWritten += size;
    return 1;
}

// Serializes `working` (non-incremental: a complete, self-contained file)
// into `sink`. Caller holds the gate.
core::Status saveTo(FPDF_DOCUMENT working, IPdfByteSink& sink) {
    SinkWriter writer;
    writer.version = 1;
    writer.WriteBlock = &writeToSink;
    writer.sink = &sink;

    const bool saved = FPDF_SaveAsCopy(working, &writer, FPDF_NO_INCREMENTAL) != 0;
    // Checked BEFORE `saved`: the final flush's result is dropped by PDFium.
    if (writer.error.has_value()) {
        return std::unexpected(std::move(*writer.error));
    }
    if (writer.threw) {
        return std::unexpected(internalError("the output sink threw an exception"));
    }
    if (!saved || writer.bytesWritten == 0) {
        return std::unexpected(internalError("PDFium failed to serialize the document"));
    }
    return core::ok();
}

core::Status verifyPageCount(FPDF_DOCUMENT working, std::size_t expected, const char* step) {
    const int count = FPDF_GetPageCount(working);
    if (count < 0 || static_cast<std::size_t>(count) != expected) {
        return std::unexpected(internalError(std::string("working document has ") +
                                             std::to_string(count) + " pages after " + step +
                                             ", expected " + std::to_string(expected)));
    }
    return core::ok();
}

core::Status assemblePreserveBase(const PdfiumDocument& base,
                                  const std::vector<ResolvedPage>& pages,
                                  IPdfByteSink& sink) {
    const std::size_t baseCount = base.info().pageCount;
    const std::size_t finalCount = pages.size();

    // A private working copy of the exact bytes the live base was opened
    // from (same inode via the shared source), decrypted with the same
    // password.
    FPDF_FILEACCESS access = base.fileSource()->fileAccess();
    ScopedDocument working(FPDF_LoadCustomDocument(&access, base.openPassword().c_str()));
    if (working.get() == nullptr) {
        const core::Error cause = pdfiumLoadError(static_cast<int>(FPDF_GetLastError()),
                                                  !base.openPassword().empty());
        return std::unexpected(core::makeError(cause.code,
                                               "could not reopen the base document: " + cause.message,
                                               "pdf"));
    }
    if (auto counted = verifyPageCount(working.get(), baseCount, "reopening"); !counted.has_value()) {
        return counted;
    }

    // workingIndex[i]: where final page i lives in the working document
    // before the move. First occurrences of base pages stay in place.
    std::vector<int> workingIndex(finalCount, -1);
    std::vector<bool> reused(baseCount, false);
    struct ImportGroup {
        const PdfiumDocument* source = nullptr;
        std::vector<int> sourceIndices;
        std::vector<std::size_t> finalPositions;
    };
    std::vector<ImportGroup> groups;
    for (std::size_t i = 0; i < finalCount; ++i) {
        const ResolvedPage& page = pages[i];
        const auto baseIndex = static_cast<std::size_t>(page.sourceIndex);
        if (page.source == &base && !reused[baseIndex]) {
            reused[baseIndex] = true;
            workingIndex[i] = page.sourceIndex;
            continue;
        }
        ImportGroup* group = nullptr;
        for (ImportGroup& candidate : groups) {
            if (candidate.source == page.source) {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr) {
            groups.push_back(ImportGroup{page.source, {}, {}});
            group = &groups.back();
        }
        group->sourceIndices.push_back(page.sourceIndex);
        group->finalPositions.push_back(i);
    }

    // Append the imports after the base pages.
    std::size_t total = baseCount;
    for (const ImportGroup& group : groups) {
        if (total + group.sourceIndices.size() > kMaxPages) {
            return std::unexpected(invalidArgument("too many pages in the assembly request"));
        }
        if (auto imported = importPages(working.get(), *group.source, group.sourceIndices,
                                        static_cast<int>(total));
            !imported.has_value()) {
            return imported;
        }
        for (std::size_t k = 0; k < group.finalPositions.size(); ++k) {
            workingIndex[group.finalPositions[k]] = static_cast<int>(total + k);
        }
        total += group.sourceIndices.size();
    }
    if (auto counted = verifyPageCount(working.get(), total, "importing"); !counted.has_value()) {
        return counted;
    }

    // Final order to the front in one move. FPDF_MovePages requires the
    // destination to be within [0, count - len]; 0 always is, including
    // when every page is part of the final order.
    bool inPlace = true;
    for (std::size_t i = 0; i < finalCount; ++i) {
        inPlace = inPlace && workingIndex[i] == static_cast<int>(i);
    }
    if (!inPlace &&
        FPDF_MovePages(working.get(), workingIndex.data(), static_cast<unsigned long>(finalCount), 0) == 0) {
        return std::unexpected(internalError("PDFium failed to reorder the working document"));
    }

    // Drop the leftovers (base pages not in the final list) from the end
    // backwards, so the remaining indices stay valid.
    for (std::size_t index = total; index > finalCount; --index) {
        FPDFPage_Delete(working.get(), static_cast<int>(index - 1));
    }
    if (auto counted = verifyPageCount(working.get(), finalCount, "deleting leftovers");
        !counted.has_value()) {
        return counted;
    }

    if (auto viewed = applyViews(working.get(), pages); !viewed.has_value()) {
        return viewed;
    }
    return saveTo(working.get(), sink);
}

core::Status assembleFresh(const std::vector<ResolvedPage>& pages, IPdfByteSink& sink) {
    ScopedDocument working(FPDF_CreateNewDocument());
    if (working.get() == nullptr) {
        return std::unexpected(core::makeError(core::ErrorCode::OutOfMemory,
                                               "PDFium could not create a new document", "pdf"));
    }

    // One import per run of consecutive pages from the same source, in
    // final order, so no reordering is needed afterwards.
    std::size_t inserted = 0;
    std::size_t runStart = 0;
    while (runStart < pages.size()) {
        std::size_t runEnd = runStart;
        std::vector<int> indices;
        while (runEnd < pages.size() && pages[runEnd].source == pages[runStart].source) {
            indices.push_back(pages[runEnd].sourceIndex);
            ++runEnd;
        }
        if (auto imported = importPages(working.get(), *pages[runStart].source, indices,
                                        static_cast<int>(inserted));
            !imported.has_value()) {
            return imported;
        }
        inserted += indices.size();
        runStart = runEnd;
    }
    if (auto counted = verifyPageCount(working.get(), pages.size(), "importing"); !counted.has_value()) {
        return counted;
    }

    if (auto viewed = applyViews(working.get(), pages); !viewed.has_value()) {
        return viewed;
    }
    return saveTo(working.get(), sink);
}

} // namespace

core::Status assembleWithPdfium(const PdfiumEngine& engine,
                                const PdfAssemblyRequest& request,
                                IPdfByteSink& sink) {
    try {
        // Public entry operation: ONE gate acquisition for the whole
        // assembly (see the gate strategy at the top of this file). Every
        // helper above assumes the gate is held and never acquires it.
        return globalPdfiumCallGate().invoke([&]() -> core::Status {
            const auto pages = resolvePages(engine, request);
            if (!pages.has_value()) {
                return std::unexpected(pages.error());
            }
            switch (request.mode) {
                case PdfAssemblyRequest::Mode::PreserveBase: {
                    const auto base = ownedDocument(engine, request.base, "base");
                    if (!base.has_value()) {
                        return std::unexpected(base.error());
                    }
                    return assemblePreserveBase(**base, *pages, sink);
                }
                case PdfAssemblyRequest::Mode::Fresh:
                    // `base` is ignored: a Fresh document has no base.
                    return assembleFresh(*pages, sink);
            }
            return std::unexpected(invalidArgument("unknown assembly mode"));
        });
    } catch (const std::bad_alloc&) {
        return std::unexpected(core::makeError(core::ErrorCode::OutOfMemory,
                                               "out of memory while assembling the document", "pdf"));
    } catch (const std::exception&) {
        return std::unexpected(internalError("unexpected failure while assembling the document"));
    }
}

} // namespace rivet::pdf
