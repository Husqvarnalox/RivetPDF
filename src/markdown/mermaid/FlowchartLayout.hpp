// SPDX-License-Identifier: MPL-2.0
#pragma once

// Layered (Sugiyama-style) layout of a Mermaid flowchart (ADR-0032):
//
//   1. node sizes from the measured labels and the shape;
//   2. cycles broken by reversing DFS back edges (drawn in their original
//      direction); self-loops are drawn as small loops beside the node;
//   3. longest-path ranking (labelled edges span at least two ranks so the
//      label gets its own slot), sources pulled down next to their targets;
//   4. edges spanning several ranks get one dummy node per crossed rank;
//   5. crossing reduction: barycentric down/up sweeps plus adjacent
//      transpositions, bounded iterations, best order kept; subgraph members
//      stay contiguous in every rank;
//   6. coordinates: per-rank least-squares placement towards the neighbours'
//      positions under minimum-separation constraints (pool adjacent
//      violators), a few sweeps; ranks stacked with label/cluster room;
//   7. polyline edges from border to border through the dummies, arrowheads,
//      labels centred on their dummy; subgraph boxes around their members.
//
// The intermediate FlowLayout (geometry only) is public so tests and hit
// testing can use it; flowchartScene() turns it into paint primitives.
// Complexity: O((V + D) log) per sweep with D dummy nodes (bounded by
// MermaidLimits::maxDummies); a fixed number of sweeps. Pure, thread-safe.

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

struct FlowNodeBox {
    core::Rect rect;
    std::size_t rank = 0;
    std::size_t order = 0; // position within the rank
};

struct FlowEdgePath {
    // From the source node's border to the target node's border (the arrow
    // tip when the edge has an arrow), in the edge's own direction.
    std::vector<core::Point> points;
    bool hasLabel = false;
    core::Rect labelRect; // label box (centre = label anchor) when hasLabel
};

struct FlowClusterBox {
    core::Rect rect;
};

struct FlowLayout {
    std::vector<FlowNodeBox> nodes;       // parallel to Flowchart::nodes
    std::vector<FlowEdgePath> edges;      // parallel to Flowchart::edges
    std::vector<FlowClusterBox> clusters; // parallel to Flowchart::subgraphs
    std::size_t rankCount = 0;
    std::size_t crossings = 0; // edge crossings between adjacent ranks after ordering
    double width = 0.0;
    double height = 0.0;
};

std::expected<FlowLayout, std::string> layoutFlowchart(const Flowchart& chart, const ITextMeasurer& measurer,
                                                       const DiagramStyle& style = {},
                                                       const MermaidLimits& limits = {});

DiagramScene flowchartScene(const Flowchart& chart, const FlowLayout& layout, const ITextMeasurer& measurer,
                            const DiagramStyle& style = {});

} // namespace rivet::markdown::mermaid
