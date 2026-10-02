// SPDX-License-Identifier: MPL-2.0
#pragma once

// PDFium implementation of the Phase 5 content model (PdfContent.hpp,
// ADR-0014..0017): extraction of a page's top-level objects, the
// regeneration fidelity probe, and the application of a
// PdfPageContentEdits to a page. INTERNAL to src/pdf/pdfium/ (the only place
// FPDF_* may appear).
//
// Gate discipline: NOTHING here acquires the process-wide PDFium call gate.
// Every function must be called from inside the caller's single acquisition
// (the mutex is non-recursive).
//
// Mutation discipline: the LIVE FPDF_DOCUMENT of a PdfiumDocument is never
// touched. Content is read from a private never-rendered copy and edited on
// scratch documents (display) or on the assembly's working document (save).

#include "core/Error.hpp"
#include "pdf/BundledFonts.hpp"
#include "pdf/PdfContent.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fpdfview.h"

namespace rivet::pdf::internal {

// The bundled fonts (ADR-0016) loaded into ONE FPDF_DOCUMENT, each at most
// once (FPDFText_LoadFont embeds the whole subset program). The handles are
// closed in the destructor, which must run before the document is closed and
// inside the gate.
class FontCache {
public:
    explicit FontCache(FPDF_DOCUMENT document) : document_(document) {}
    ~FontCache();
    FontCache(const FontCache&) = delete;
    FontCache& operator=(const FontCache&) = delete;

    // The loaded font, or nullptr when PDFium rejects the font program.
    FPDF_FONT get(PdfBundledFont face);

private:
    FPDF_DOCUMENT document_;
    std::array<FPDF_FONT, 6> fonts_{};
};

// What applying content edits produced; see PdfAssembledPageContent. The
// vectors have one entry per top-level object of the edited page, in z-order.
struct ContentApplyResult {
    std::vector<PdfContentOrigin> origins;
    std::vector<std::uint64_t> blockTags;
    // Tags of the text blocks that had to use their fallback font.
    std::unordered_set<std::uint64_t> substitutedTags;
};

// Top-level objects of a loaded page (origins are Source(i), blockTag 0,
// regenerationSafe left true; the caller sets those). More than
// kMaxContentObjectsPerPage objects: `truncated`, no objects.
core::Result<PdfPageContent> extractPageContent(FPDF_PAGE page);

struct RegenerationProbe {
    bool safe = true;
    std::string issue; // short, no document content
};

// The ADR-0017 fidelity probe for page `pageIndex` of `source` (a private,
// never-rendered document): regenerates the content of a scratch copy and
// compares structure and rendering. `reference` is ANOTHER private copy of
// the same file that may be rendered: the scratch copy lacks document-level
// state (optional-content configuration, output intents, ...), so the
// regenerated page is compared with the ORIGINAL page as the full document
// shows it, which also catches content that would turn visible. Never fails:
// a probe that cannot run reports the page as unsafe.
RegenerationProbe probeRegeneration(FPDF_DOCUMENT source, FPDF_DOCUMENT reference, int pageIndex);

// Applies `edits` to the loaded `page` of `document` (see PdfContent.hpp for
// the semantics) and regenerates its content streams. `fonts` must belong to
// `document`. The edits are validated against the page's object count first.
// On error the page is left in an unspecified state: callers discard it.
core::Result<ContentApplyResult> applyContentEdits(FPDF_DOCUMENT document,
                                                   FPDF_PAGE page,
                                                   const PdfPageContentEdits& edits,
                                                   FontCache& fonts);

// An edited page, materialized for display: source page imported into a
// scratch document, edits applied, serialized to memory and reloaded, so
// what is shown is exactly what a save writes.
class MaterializedPage {
public:
    ~MaterializedPage();
    MaterializedPage(const MaterializedPage&) = delete;
    MaterializedPage& operator=(const MaterializedPage&) = delete;

    FPDF_PAGE page() const { return page_; }
    const ContentApplyResult& applied() const { return applied_; }
    std::size_t byteSize() const { return buffer_.size(); }

private:
    friend core::Result<std::unique_ptr<MaterializedPage>> materializePage(FPDF_DOCUMENT source,
                                                                           int pageIndex,
                                                                           const PdfPageContentEdits& edits);
    MaterializedPage() = default;

    std::vector<std::uint8_t> buffer_; // must outlive document_ (declared first)
    FPDF_DOCUMENT document_ = nullptr;
    FPDF_PAGE page_ = nullptr;
    ContentApplyResult applied_;
};

// `source`: a private never-rendered document. The edits are validated here;
// refusing pages that failed the regeneration probe is the CALLER's job (it
// owns the probe cache).
core::Result<std::unique_ptr<MaterializedPage>> materializePage(FPDF_DOCUMENT source,
                                                                int pageIndex,
                                                                const PdfPageContentEdits& edits);

// Per-PdfiumDocument content state: the private content reader's results and
// the materialization cache. Accessed only under the gate.
class ContentState {
public:
    ContentState() = default;
    ContentState(const ContentState&) = delete;
    ContentState& operator=(const ContentState&) = delete;
    ~ContentState() = default;

    // Closes the reader and drops every cached page. Inside the gate.
    void closeAll();

    FPDF_DOCUMENT reader() const { return reader_; }
    // The reader (never rendered) and the probe's reference copy (rendered
    // by the probe), both private copies of the same file.
    void setReader(FPDF_DOCUMENT reader, FPDF_DOCUMENT probeReference) {
        reader_ = reader;
        probeReference_ = probeReference;
    }

    // The page as stored: extraction + probe, cached (bounded).
    core::Result<PdfPageContentPtr> sourcePage(std::size_t pageIndex);

    // The materialized page for (pageIndex, edits), cached by the identity of
    // the edits object. Fails with InvalidArgument when the page is not
    // regeneration-safe. The returned pointer is valid until the next call
    // on this state.
    struct Entry {
        std::size_t pageIndex = 0;
        const PdfPageContentEdits* key = nullptr;
        std::weak_ptr<const PdfPageContentEdits> alive;
        std::unique_ptr<MaterializedPage> page;
        PdfPageContentPtr content; // lazily extracted
    };
    core::Result<Entry*> materialized(std::size_t pageIndex, const PdfPageContentEditsPtr& edits);

    // The (lazily extracted) content of a materialized entry with origins,
    // blockTags, fontSubstituted and the source page's probe result filled.
    core::Result<PdfPageContentPtr> editedContent(Entry& entry);

private:
    static constexpr std::size_t kSourceCacheCapacity = 16;
    static constexpr std::size_t kMaterializedCapacity = 8;
    static constexpr std::size_t kMaterializedByteBudget = 256u * 1024u * 1024u;

    FPDF_DOCUMENT reader_ = nullptr;
    FPDF_DOCUMENT probeReference_ = nullptr;
    std::unordered_map<std::size_t, PdfPageContentPtr> source_;
    std::deque<std::size_t> sourceOrder_;
    std::list<Entry> entries_; // most recently used first
    std::size_t entryBytes_ = 0;
};

} // namespace rivet::pdf::internal
