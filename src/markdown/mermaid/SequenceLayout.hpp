// SPDX-License-Identifier: MPL-2.0
#pragma once

// Layout of a Mermaid sequence diagram (ADR-0032): participant boxes along
// the top (mirrored at the bottom), dashed lifelines, one row per message or
// note in source order, loop/alt/opt/... frames with title tabs and `else`
// dividers. Columns are spaced so that boxes, message labels and notes fit
// (one left-to-right pass over the span constraints). Pure, thread-safe.

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "markdown/MarkdownLayout.hpp"
#include "markdown/mermaid/DiagramScene.hpp"
#include "markdown/mermaid/MermaidModel.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

namespace rivet::markdown::mermaid {

struct SeqParticipantBox {
    core::Rect top;
    core::Rect bottom;
    double x = 0.0; // lifeline
};

struct SeqMessageGeometry {
    std::size_t event = 0;            // index into SequenceDiagram::events
    std::vector<core::Point> points;  // from sender to receiver (arrow tip last)
    core::Rect labelRect;             // empty when the message has no text
};

struct SeqNoteGeometry {
    std::size_t event = 0;
    core::Rect rect;
};

struct SeqFrameGeometry {
    std::size_t event = 0;            // the block start
    std::size_t depth = 0;            // 0 = outermost
    core::Rect rect;
    std::vector<double> dividers;     // y of each else/and/option line
};

struct SequenceLayout {
    std::vector<SeqParticipantBox> participants;
    std::vector<SeqMessageGeometry> messages; // source order
    std::vector<SeqNoteGeometry> notes;       // source order
    std::vector<SeqFrameGeometry> frames;     // by start event
    double width = 0.0;
    double height = 0.0;
};

std::expected<SequenceLayout, std::string> layoutSequence(const SequenceDiagram& diagram,
                                                          const ITextMeasurer& measurer,
                                                          const DiagramStyle& style = {},
                                                          const MermaidLimits& limits = {});

DiagramScene sequenceScene(const SequenceDiagram& diagram, const SequenceLayout& layout,
                           const ITextMeasurer& measurer, const DiagramStyle& style = {});

} // namespace rivet::markdown::mermaid
