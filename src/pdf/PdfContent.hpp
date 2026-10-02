// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Matrix.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Page content objects and content edits (Phase 5, ADR-0014..0017).
//
// Everything here is backend-neutral data in the SOURCE page's PDF USER
// space (points, origin bottom-left, y-up, before /Rotate and /CropBox). The
// editor maps it to display space through PdfPageView (PdfPageGeometry.hpp);
// no backend handle (FPDF_PAGEOBJECT, ...) ever leaves the backend.
//
// The model is "source page + immutable edit description": a page's content
// is what its file says, transformed by a PdfPageContentEdits value. The
// backend applies the SAME edit description for display (render, text,
// object extraction) and for saving, so what is shown is what is written.

namespace rivet::pdf {

// --- Bounds (defensive limits; ADR-0014 "Security") -------------------------

// Pages with more top-level objects are listed as truncated and read-only.
inline constexpr std::size_t kMaxContentObjectsPerPage = 20000;
// Longest text a single block edit may carry (UTF-8 bytes).
inline constexpr std::size_t kMaxTextBlockBytes = 64 * 1024;
// Most text blocks a page's edits may create or replace.
inline constexpr std::size_t kMaxTextBlocksPerPage = 4096;
// Replacement images: per-side pixel limit, total pixel limit, encoded size.
inline constexpr std::uint32_t kMaxImageSide = 10000;
inline constexpr std::uint64_t kMaxImagePixels = 50'000'000;
inline constexpr std::size_t kMaxImageEncodedBytes = 64u * 1024u * 1024u;
// Font sizes a text edit accepts (effective size, user-space points).
inline constexpr double kMinFontSize = 1.0;
inline constexpr double kMaxFontSize = 500.0;

// --- Extracted objects -------------------------------------------------------

enum class PdfContentObjectType : std::uint8_t { Text, Image, Path, Shading, Form, Unknown };

// Where an object of an (edited) page comes from.
//   Source:  the top-level object at `sourceIndex` of the SOURCE page as
//            stored in its file (possibly modified in place by an edit).
//   Created: appended by the text block edit whose tag is `tag`.
struct PdfContentOrigin {
    enum class Kind : std::uint8_t { Source, Created };
    Kind kind = Kind::Source;
    std::uint32_t sourceIndex = 0; // Source only
    std::uint64_t tag = 0;         // Created only
    bool operator==(const PdfContentOrigin&) const = default;
};

struct PdfFontInfo {
    std::string baseName;   // /BaseFont (may carry a subset prefix "ABCDEF+")
    std::string familyName;
    bool embedded = false;
    bool subset = false;    // embedded subset (six-capital-letter prefix)
    bool type3 = false;
    bool standard14 = false; // one of the 14 standard fonts, not embedded
    bool bold = false;
    bool italic = false;
    bool monospace = false;
    bool serif = false;
    bool operator==(const PdfFontInfo&) const = default;
};

// One top-level page object (z-order position `index`, 0 = bottom). Objects
// nested in Form XObjects are never listed individually (ADR-0014).
struct PdfContentObject {
    std::uint32_t index = 0;
    PdfContentOrigin origin;
    // Non-zero: the object belongs to the text block edit with this tag
    // (a reused member of the block or a line it created).
    std::uint64_t blockTag = 0;
    PdfContentObjectType type = PdfContentObjectType::Unknown;

    // User space. Text: the text matrix (CTM applied; translation = baseline
    // origin of the first glyph). Image: maps the unit square onto the page.
    // Path/Form/Shading: the object's matrix.
    core::Matrix matrix;
    PdfBox bounds;                  // axis-aligned, user space
    std::array<PdfPoint, 4> quad{}; // tight (rotated) bounds, user space
    bool hasClip = false;           // a clip path applies to the object
    bool hasMarkedContent = false;  // e.g. /MCID (tagged PDF)

    // Text.
    std::string text;               // UTF-8 as extracted (may be empty)
    bool textUnmappable = false;    // glyphs without a Unicode mapping
    double fontSize = 0.0;          // EFFECTIVE size in user-space points
    PdfFontInfo font;
    int renderMode = 0;             // 0..7 (3 = invisible)
    bool fontSubstituted = false;   // produced by an edit with a fallback font

    // Colors (sRGB approximation; nullopt = none or not representable).
    std::optional<PdfColor> fill;
    std::optional<PdfColor> stroke;

    // Image.
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;

    // Path.
    std::uint32_t segmentCount = 0;

    // Form XObject: number of objects inside (not individually editable).
    std::uint32_t childCount = 0;
};

// The top-level objects of one (possibly edited) page.
struct PdfPageContent {
    std::vector<PdfContentObject> objects; // z-order, bottom first
    // More than kMaxContentObjectsPerPage objects: `objects` is empty and the
    // page is read-only.
    bool truncated = false;
    // Result of the regeneration fidelity probe on the SOURCE page
    // (ADR-0017): false = rewriting this page's content streams would change
    // or lose content, so the page's objects are read-only. `issue` is a
    // short human-readable reason (no document content).
    bool regenerationSafe = true;
    std::string regenerationIssue;
};
using PdfPageContentPtr = std::shared_ptr<const PdfPageContent>;

// --- Edits -------------------------------------------------------------------

// Fonts Rivet ships (Apache-2.0, Latin/Latin-1/Latin Ext-A/Greek/Cyrillic
// subsets; ADR-0016). Used for Add Text and as the fallback when an edited
// text's own font cannot encode the new text.
enum class PdfBundledFont : std::uint8_t {
    SansRegular,
    SansBold,
    SerifRegular,
    SerifBold,
    MonoRegular,
    MonoBold,
};

// Which font a text block edit uses.
//   FromObject: the font of source object `sourceIndex` of the same page,
//               when it can encode the whole text; otherwise the backend
//               substitutes `fallback` for the WHOLE block (reported through
//               PdfContentObject::fontSubstituted).
//   Bundled:    always `fallback`.
struct PdfFontRef {
    enum class Kind : std::uint8_t { FromObject, Bundled };
    Kind kind = Kind::Bundled;
    std::uint32_t sourceIndex = 0;
    PdfBundledFont fallback = PdfBundledFont::SansRegular;
    bool operator==(const PdfFontRef&) const = default;
};

// A text block written by Rivet: new text (Add Text) or the replacement of
// existing text objects (`members`).
//
// Block frame: x along the baseline, y up, origin at the start of the FIRST
// line's baseline; `placement` maps it to user space and must be rigid
// (rotation + translation; validate()). Line k (0-based) starts at
// (0, -k * lineAdvance). Text is laid out by layoutTextBlock
// (PdfTextLayout.hpp) with the font's real advances: '\n' breaks lines,
// wrapWidth > 0 wraps at whitespace. Nothing is ever clipped: a block grows
// downwards as far as its text needs (ADR-0015 "Overflow").
struct PdfTextBlockEdit {
    std::uint64_t tag = 0;              // non-zero, unique within the page's edits
    std::vector<std::uint32_t> members; // sorted source indices replaced (empty = new text)
    std::string text;                   // UTF-8
    PdfFontRef font;
    double fontSize = 12.0;             // effective size, user-space points
    PdfColor color;                     // fill
    core::Matrix placement;
    double wrapWidth = 0.0;             // points; <= 0 = no wrapping
    double lineAdvance = 14.4;          // points between baselines, > 0
};

// Decoded or pass-through image data for a replacement.
//   Jpeg: `bytes` is a complete JPEG file (embedded as /DCTDecode as is).
//   Bgra: 8-bit straight-alpha BGRA rows of `stride` bytes.
struct PdfImageData {
    enum class Format : std::uint8_t { Jpeg, Bgra };
    Format format = Format::Bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0; // Bgra only
    std::vector<std::uint8_t> bytes;
};

// A change of one existing (source) top-level object.
struct PdfObjectEdit {
    std::uint32_t sourceIndex = 0;
    bool remove = false;
    // User-space transform applied AFTER the object's own matrix
    // (new = transform * old); nullopt = unchanged.
    std::optional<core::Matrix> transform;
    // Images only: the new pixels, fitted into the object's (transformed)
    // frame keeping their aspect ratio, centered.
    std::shared_ptr<const PdfImageData> replaceImage;
};

// All content edits of one page, against its SOURCE page. Immutable once
// shared (the page model and the backend caches hold it by pointer).
//
// Application order (ADR-0017): object edits (removals, transforms, image
// replacements) in source order, then text blocks in vector order. Objects a
// text block creates are appended on top of the page (new content is always
// in front); members reused by a block keep their z-position.
struct PdfPageContentEdits {
    std::vector<PdfObjectEdit> objects;       // sorted by sourceIndex, unique
    std::vector<PdfTextBlockEdit> textBlocks; // creation order
    bool empty() const { return objects.empty() && textBlocks.empty(); }
};
using PdfPageContentEditsPtr = std::shared_ptr<const PdfPageContentEdits>;

// Structural validation against a source page with `sourceObjectCount`
// top-level objects: sorted/unique/in-range indices, a block's members are
// not removed or claimed by another block and are not transformed by an
// object edit, tags non-zero and unique, finite rigid placements, font sizes
// in [kMinFontSize, kMaxFontSize], positive line advance, the size limits
// above, image data consistent with its format. InvalidArgument otherwise.
core::Status validate(const PdfPageContentEdits& edits, std::size_t sourceObjectCount);

// Per output page of an assembly with content edits: the origin of every
// top-level object of the written page, in z-order (index i = object i of
// the saved page). Lets the editor re-key object identities after a save
// without re-reading the file. `blockTags[i]` mirrors
// PdfContentObject::blockTag.
struct PdfAssembledPageContent {
    std::vector<PdfContentOrigin> origins;
    std::vector<std::uint64_t> blockTags;
};

} // namespace rivet::pdf
