// SPDX-License-Identifier: MPL-2.0
#pragma once

// Parsed Mermaid diagrams (ADR-0032): the flowchart/graph subset and the
// sequenceDiagram subset. Produced by parseMermaid() (MermaidParser.hpp),
// consumed by the layouts. Plain values; no references into the source.

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rivet::markdown::mermaid {

struct MermaidLimits {
    std::size_t maxSourceBytes = 64 * 1024;
    std::size_t maxNodes = 500;   // flowchart nodes / sequence participants
    std::size_t maxEdges = 2000;  // flowchart edges / sequence messages, notes and blocks
    std::size_t maxDummies = 20000; // flowchart layout: long-edge segments
    std::size_t maxBlockDepth = 16; // sequence loop/alt/opt nesting
};

// ---------------------------------------------------------------- flowchart

enum class FlowDirection : std::uint8_t { TopBottom, BottomTop, LeftRight, RightLeft };

enum class NodeShape : std::uint8_t { Rect, Round, Stadium, Diamond, Circle };

enum class EdgeStroke : std::uint8_t { Solid, Dotted, Thick };

struct FlowNode {
    std::string id;
    std::vector<std::string> label; // lines; the id when no label was given
    NodeShape shape = NodeShape::Rect;
    int subgraph = -1;              // index into Flowchart::subgraphs, -1 = none
};

struct FlowEdge {
    std::size_t from = 0;
    std::size_t to = 0;
    std::vector<std::string> label; // lines; empty = no label
    EdgeStroke stroke = EdgeStroke::Solid;
    bool arrow = true;
};

struct FlowSubgraph {
    std::string id;
    std::string title;
};

struct Flowchart {
    FlowDirection direction = FlowDirection::TopBottom;
    std::vector<FlowNode> nodes;
    std::vector<FlowEdge> edges;
    std::vector<FlowSubgraph> subgraphs;
};

// ----------------------------------------------------------------- sequence

struct Participant {
    std::string id;
    std::string label;
    bool actor = false;
};

enum class ArrowHead : std::uint8_t { None, Filled, Cross, Open };

struct SeqMessage {
    std::size_t from = 0;
    std::size_t to = 0;
    std::string text;
    bool dashed = false;
    ArrowHead head = ArrowHead::Filled;
    std::size_t number = 0; // 1-based when autonumber is on, else 0
};

enum class NotePlacement : std::uint8_t { LeftOf, RightOf, Over };

struct SeqNote {
    NotePlacement placement = NotePlacement::Over;
    std::size_t first = 0;
    std::size_t last = 0; // == first unless "over A,B"
    std::string text;
};

enum class SeqBlockKind : std::uint8_t { Loop, Alt, Opt, Par, Critical, Break, Rect };

struct SeqBlockStart {
    SeqBlockKind kind = SeqBlockKind::Loop;
    std::string label;
};

// `else` (alt), `and` (par), `option` (critical).
struct SeqBlockElse {
    std::string label;
};

struct SeqBlockEnd {};

using SeqEvent = std::variant<SeqMessage, SeqNote, SeqBlockStart, SeqBlockElse, SeqBlockEnd>;

struct SequenceDiagram {
    std::vector<Participant> participants;
    std::vector<SeqEvent> events;
    bool autonumber = false;
};

using MermaidDiagram = std::variant<Flowchart, SequenceDiagram>;

} // namespace rivet::markdown::mermaid
