// SPDX-License-Identifier: MPL-2.0
#pragma once

// Paint-ready output of the Mermaid renderer (ADR-0032): a flat, ordered list
// of primitives that the Markdown painter and the PDF exporter both draw, so
// preview, print and export match. Colors are semantic roles (the theme maps
// them); text carries a font role and size that map to a markdown TextStyle
// through diagramTextStyle(), which is also the style the layout measured.
//
// Coordinates: diagram space, y grows downward, origin top-left, units of the
// ITextMeasurer (typically points). Items are listed in paint order.

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "markdown/MarkdownLayout.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rivet::markdown::mermaid {

enum class DiagramColor : std::uint8_t {
    Text,          // labels
    NodeFill,
    NodeStroke,
    EdgeStroke,    // edges, messages, arrowheads
    LabelFill,     // background behind edge labels
    ClusterFill,   // subgraph boxes
    ClusterStroke,
    NoteFill,
    NoteStroke,
    FrameStroke,   // loop/alt/opt frames and their title tabs
    FrameFill,     // frame title tab
    Lifeline,
    NumberFill,    // autonumber badges
    NumberText,
};

enum class DiagramFont : std::uint8_t { Regular, Bold };

enum class DashStyle : std::uint8_t { Solid, Dashed, Dotted };

struct DiagramText {
    std::string utf8;     // one line, never empty
    core::Point origin;   // left end of the baseline
    double width = 0.0;   // measured advance
    DiagramFont font = DiagramFont::Regular;
    double size = 14.0;
    DiagramColor color = DiagramColor::Text;
};

struct DiagramRect {
    core::Rect rect;
    double cornerRadius = 0.0;
    bool filled = true;
    DiagramColor fill = DiagramColor::NodeFill;
    bool stroked = true;
    DiagramColor stroke = DiagramColor::NodeStroke;
    double strokeWidth = 1.0;
    DashStyle dash = DashStyle::Solid;
};

struct DiagramPolyline {
    std::vector<core::Point> points; // >= 2
    DiagramColor color = DiagramColor::EdgeStroke;
    double width = 1.0;
    DashStyle dash = DashStyle::Solid;
};

// A path of absolute segments (diamonds, circles, arrowheads, crosses).
struct PathSegment {
    enum class Kind : std::uint8_t { MoveTo, LineTo, CubicTo, Close };
    Kind kind = Kind::MoveTo;
    core::Point p;  // end point (MoveTo/LineTo/CubicTo)
    core::Point c1; // CubicTo controls
    core::Point c2;
};

struct DiagramPath {
    std::vector<PathSegment> segments;
    bool filled = true;
    DiagramColor fill = DiagramColor::NodeFill;
    bool stroked = true;
    DiagramColor stroke = DiagramColor::NodeStroke;
    double strokeWidth = 1.0;
    DashStyle dash = DashStyle::Solid;
};

using DiagramItem = std::variant<DiagramRect, DiagramPath, DiagramPolyline, DiagramText>;

struct DiagramScene {
    double width = 0.0;
    double height = 0.0;
    std::vector<DiagramItem> items; // paint order
};

// Fonts and spacing of the renderer. Defaults suit a 15pt body text.
struct DiagramStyle {
    double fontSize = 13.0;
    double lineHeightFactor = 1.3;
    double margin = 8.0;
    double nodePadX = 14.0;
    double nodePadY = 9.0;
    double nodeSep = 28.0;      // between neighbours in a rank
    double rankSep = 44.0;      // between ranks
    double edgeSep = 14.0;      // spacing reserved per long-edge dummy
    double clusterPad = 12.0;
    double strokeWidth = 1.2;
    double thickStrokeWidth = 2.6;
    double arrowLength = 8.0;
    double arrowWidth = 7.0;
    // Sequence diagrams.
    double actorGap = 36.0;     // minimum gap between participant boxes
    double messageGap = 14.0;   // vertical space after a message
    double noteWidthMin = 60.0;
    double selfLoopWidth = 34.0;
};

// The TextStyle a diagram text run is measured and painted with.
inline TextStyle diagramTextStyle(DiagramFont font, double size) {
    TextStyle style;
    style.kind = TextKind::Body;
    style.bold = font == DiagramFont::Bold;
    style.size = size;
    return style;
}

} // namespace rivet::markdown::mermaid
