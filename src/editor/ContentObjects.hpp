// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/StrongId.hpp"
#include "core/geometry/Matrix.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfContent.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Page content as the editor and the UI see it (ADR-0014): display-space
// views of the backend's objects plus reconstructed text blocks (ADR-0015).
// Everything here is immutable value data computed by ContentService.

namespace rivet::editor {

// What the user may do with an object or a text block (ADR-0014 table).
enum class ContentCapability : std::uint8_t {
    ReadOnly,      // nothing (reason tells why)
    MoveOnly,      // move, delete
    Replaceable,   // + retype, but a bundled substitute font will be used
    FullyEditable, // + retype in the object's own font
};

// One top-level object in DISPLAY space of the entry's current view
// (points, origin top-left, y-down, rotation and crop applied).
struct ContentObjectView {
    core::ObjectId id;
    std::uint32_t index = 0; // z-order position on the (edited) page, 0 = bottom
    pdf::PdfContentObjectType type = pdf::PdfContentObjectType::Unknown;
    core::Rect bounds;               // axis-aligned display bounds
    std::array<core::Point, 4> quad; // tight bounds (text/image rotation), display
    ContentCapability capability = ContentCapability::ReadOnly;
    std::string capabilityReason; // short, human-readable, never document text
    bool edited = false;          // affected by a content edit of this page
    // Text: the block it belongs to (null id when it is not in a block).
    core::ObjectId block;
    // Image: pixel size (for the properties bar).
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
    // Copy of the backend record (user space) for commands and properties.
    pdf::PdfContentObject source;
};

// A line of a reconstructed text block: the member objects in reading
// order and the display-space baseline.
struct TextBlockLine {
    std::vector<core::ObjectId> members;
    std::string text; // UTF-8, joined member texts
    core::Point baselineStart; // display
    core::Point baselineEnd;   // display
};

// A reconstructed text block (ADR-0015). Identity = the ObjectId of its
// first member. Blocks created/edited by Rivet carry `tag` (the edit tag).
struct TextBlockView {
    core::ObjectId id;
    std::uint64_t tag = 0;
    std::vector<TextBlockLine> lines;   // top to bottom
    std::string text;                   // UTF-8, lines joined with '\n'
    core::Rect bounds;                  // display, union of member bounds
    std::array<core::Point, 4> quad;    // display, rotated frame
    double rotationDegrees = 0.0;       // display-space rotation of the baseline, clockwise
    double fontSize = 0.0;              // effective size (user-space points)
    double lineAdvance = 0.0;           // measured baseline step (user-space points)
    double wrapWidth = 0.0;             // user-space width (0 = single line, no wrap)
    pdf::PdfFontInfo font;              // of the first member
    pdf::PdfColor color;                // fill of the first member
    bool fontSubstituted = false;
    ContentCapability capability = ContentCapability::ReadOnly;
    std::string capabilityReason;
    bool edited = false;
};

// The resolved content of one page.
struct PageContentView {
    std::vector<ContentObjectView> objects; // z-order, bottom first
    std::vector<TextBlockView> blocks;      // content order
    bool loaded = false;        // false while the originals are still loading
    bool truncated = false;     // > kMaxContentObjectsPerPage: nothing editable
    bool regenerationSafe = true;
    std::string regenerationIssue;
};
using PageContentViewPtr = std::shared_ptr<const PageContentView>;

// Display-space outline + bounds helper: the quad of a user-space object
// mapped through the entry's view (centralized in ContentGeometry.cpp).
struct ContentHit {
    core::ObjectId id;
    bool isBlock = false; // the hit is a text block (id = block id)
};

} // namespace rivet::editor
