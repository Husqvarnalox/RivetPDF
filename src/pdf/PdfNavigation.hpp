// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rivet::pdf {

// Destination of an outline item or internal link. pageIndex is zero-based;
// point (when present) is in DISPLAYED-PAGE space (points, top-left origin,
// y-down) of the target page's NATIVE view - the same space renderPage and
// the text APIs expose. userX/userY (when hasUserPoint) carry the same
// location in PDF user space of the TARGET page, so a consumer presenting
// that page through a different view (rotate/crop in the page model) can
// re-map it with userToDisplay(view, userX, userY).
struct PdfDestination {
    std::size_t pageIndex = 0;
    bool hasPoint = false;
    core::Point point;
    enum class Fit : std::uint8_t {
        Unknown,
        XYZ,
        Fit,
        FitH,
        FitV,
        FitR,
        FitB,
        FitBH,
        FitBV,
    };
    Fit fit = Fit::Unknown;
    bool hasUserPoint = false;
    double userX = 0.0;
    double userY = 0.0;
};

// A tree of document outline items ("bookmarks"). Depth- and count-bounded
// at EXTRACTION time (limits live in the adapter), so consumers can traverse
// without additional guards. A node whose subtree was cut off by a limit
// carries truncated = true (the UI hints instead of silently omitting).
struct PdfOutlineNode {
    std::string title;                          // UTF-8
    std::optional<PdfDestination> destination;  // nullopt = no destination
    std::vector<PdfOutlineNode> children;
    bool truncated = false;
};

// One link on a page. rects are the clickable regions in displayed-page
// points (one per quad-point group; the annotation rect as fallback).
struct PdfPageLink {
    enum class Kind : std::uint8_t {
        Internal,  // navigate within the document (destination below)
        External,  // open url after explicit user interaction (scheme
                   // allow-list enforced by the consumer)
        Other,     // recognized but not acted on (e.g. embedded file)
    };
    Kind kind = Kind::Other;
    std::vector<core::Rect> rects;
    PdfDestination destination;  // Internal only
    std::string url;             // External only
};

} // namespace rivet::pdf
