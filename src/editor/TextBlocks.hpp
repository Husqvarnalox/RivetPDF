// SPDX-License-Identifier: MPL-2.0
#pragma once

// Deterministic text block reconstruction and capability classification
// (ADR-0014 table, ADR-0015 thresholds). Pure functions over the backend's
// PdfPageContent: no document access, no display mapping (everything here
// is PDF user space; ContentService maps results to display space).

#include "editor/ContentObjects.hpp"
#include "pdf/PdfContent.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rivet::editor {

// --- ADR-0015 thresholds (named so tests and the ADR agree) -------------------

inline constexpr double kLineRotationToleranceDegrees = 0.5;
inline constexpr double kLineSizeTolerance = 0.05;         // relative
inline constexpr double kLineBaselineTolerance = 0.25;     // x size
inline constexpr double kLineMaxGap = 1.5;                 // x size, along the baseline
inline constexpr double kLineMaxOverlap = 0.5;             // x size, negative gap
inline constexpr double kBlockLeftTolerance = 1.0;         // x size
inline constexpr double kBlockMinStep = 0.8;               // x size
inline constexpr double kBlockMaxStep = 2.2;               // x size
inline constexpr double kBlockStepConsistency = 0.25;      // relative to the first step
inline constexpr double kDefaultLineAdvanceFactor = 1.2;   // x size
inline constexpr double kWordGapFactor = 0.15;             // x size: a gap that reads as a space
inline constexpr double kWrapWidthSlackFactor = 0.1;       // x size added to the measured block width

struct ObjectCapabilityInfo {
    ContentCapability capability = ContentCapability::ReadOnly;
    std::string reason; // short, human readable, never document text
};

// Page-wide restriction (truncated page, failed regeneration probe): every
// object of the page is ReadOnly. nullopt when the page is editable.
std::optional<std::string> pageReadOnlyReason(const pdf::PdfPageContent& content);

// Capability of one object by ADR-0014 (without the page-wide restriction):
//   Text: invisible (render mode 3) or clipping modes (>= 4) -> ReadOnly;
//         Type3, unmappable glyphs, clip path, skewed / non-uniform / mirrored
//         matrix -> MoveOnly; substituted, non-embedded non-standard-14 or
//         subset font -> Replaceable; embedded non-subset or standard-14 ->
//         FullyEditable.
//   Image: FullyEditable (move, resize, replace, delete).
//   Path / Form / Shading: MoveOnly. Unknown: ReadOnly.
ObjectCapabilityInfo classifyObject(const pdf::PdfContentObject& object);

// Capability of every object (parallel to content.objects) including the
// page-wide restriction.
std::vector<ObjectCapabilityInfo> classifyObjects(const pdf::PdfPageContent& content);

struct ReconstructedLine {
    std::vector<std::uint32_t> members; // positions in content.objects, reading order
    std::string text;                   // UTF-8, member texts joined
    pdf::PdfPoint baselineStart;        // user space: first member's origin
    pdf::PdfPoint baselineEnd;          // user space: end of the last member along the baseline
    double width = 0.0;                 // extent along the baseline, points
};

struct ReconstructedBlock {
    std::vector<ReconstructedLine> lines; // top to bottom
    std::uint64_t tag = 0;                // non-zero: authoritative block of a text edit
    std::string text;                     // lines joined with '\n'
    double angleRadians = 0.0;            // baseline direction in user space (counter-clockwise)
    double fontSize = 0.0;                // effective, first member
    double lineAdvance = 0.0;             // measured baseline step (1.2 x size for one line)
    double wrapWidth = 0.0;               // widest line + slack for multi-line blocks, else 0
    std::array<pdf::PdfPoint, 4> frame{}; // oriented bounding box, user space
    pdf::PdfBox bounds;                   // axis-aligned union of member bounds, user space
    bool fontSubstituted = false;
    ContentCapability capability = ContentCapability::ReadOnly;
    std::string capabilityReason;

    std::uint32_t firstMember() const { return lines.front().members.front(); }
};

// Blocks of the page in content order (ordered by their first member). Every
// text object belongs to exactly one block; non-text objects to none.
// `capabilities` must be classifyObjects(content) (parallel to the objects).
std::vector<ReconstructedBlock> reconstructTextBlocks(const pdf::PdfPageContent& content,
                                                      const std::vector<ObjectCapabilityInfo>& capabilities);

} // namespace rivet::editor
