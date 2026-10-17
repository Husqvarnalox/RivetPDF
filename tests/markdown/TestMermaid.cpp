// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "MarkdownTestKit.hpp"
#include "markdown/mermaid/Mermaid.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Mermaid subset (ADR-0032): parser coverage and error fallbacks, layered
// flowchart geometry, sequence layout, scene output and limits.

using namespace rivet::markdown;
using namespace rivet::markdown::mermaid;
using rivet::core::Point;
using rivet::core::Rect;
using testkit::FixedMeasurer;

namespace {

Flowchart flow(std::string_view src) {
    auto parsed = parseMermaid(src);
    if (!parsed) throw std::runtime_error("parse failed: " + parsed.error());
    return std::get<Flowchart>(*parsed);
}

SequenceDiagram seq(std::string_view src) {
    auto parsed = parseMermaid(src);
    if (!parsed) throw std::runtime_error("parse failed: " + parsed.error());
    return std::get<SequenceDiagram>(*parsed);
}

FlowLayout layoutOf(const Flowchart& chart) {
    FixedMeasurer m;
    auto layout = layoutFlowchart(chart, m);
    if (!layout) throw std::runtime_error("layout failed: " + layout.error());
    return *layout;
}

SequenceLayout layoutOf(const SequenceDiagram& d) {
    FixedMeasurer m;
    auto layout = layoutSequence(d, m);
    if (!layout) throw std::runtime_error("layout failed: " + layout.error());
    return *layout;
}

std::size_t nodeIndex(const Flowchart& c, std::string_view id) {
    for (std::size_t i = 0; i < c.nodes.size(); ++i) {
        if (c.nodes[i].id == id) return i;
    }
    throw std::runtime_error("no node");
}

Point center(const Rect& r) { return Point(r.minX() + r.size.width / 2.0, r.minY() + r.size.height / 2.0); }

bool overlaps(const Rect& a, const Rect& b, double tol = 0.5) {
    return a.minX() < b.maxX() - tol && b.minX() < a.maxX() - tol && a.minY() < b.maxY() - tol &&
           b.minY() < a.maxY() - tol;
}

bool contains(const Rect& outer, const Rect& inner, double tol = 0.01) {
    return inner.minX() >= outer.minX() - tol && inner.maxX() <= outer.maxX() + tol &&
           inner.minY() >= outer.minY() - tol && inner.maxY() <= outer.maxY() + tol;
}

// Whether `p` lies on the outline of `shape` inscribed in `r`.
bool onBorder(NodeShape shape, const Rect& r, Point p, double tol = 0.05) {
    const Point c = center(r);
    const double hw = r.size.width / 2.0;
    const double hh = r.size.height / 2.0;
    const double dx = p.x - c.x;
    const double dy = p.y - c.y;
    switch (shape) {
    case NodeShape::Diamond: return std::abs(std::abs(dx) / hw + std::abs(dy) / hh - 1.0) < tol / std::min(hw, hh) + 1e-6;
    case NodeShape::Circle: return std::abs(std::hypot(dx / hw, dy / hh) - 1.0) < tol / std::min(hw, hh) + 1e-6;
    default: {
        const bool inside = std::abs(dx) <= hw + tol && std::abs(dy) <= hh + tol;
        const bool onEdge = std::abs(std::abs(dx) - hw) <= tol || std::abs(std::abs(dy) - hh) <= tol;
        return inside && onEdge;
    }
    }
}

void checkNoNodeOverlap(const FlowLayout& l) {
    for (std::size_t i = 0; i < l.nodes.size(); ++i) {
        for (std::size_t j = i + 1; j < l.nodes.size(); ++j) CHECK(!overlaps(l.nodes[i].rect, l.nodes[j].rect));
    }
}

void checkEndpointsOnBorders(const Flowchart& c, const FlowLayout& l) {
    for (std::size_t e = 0; e < c.edges.size(); ++e) {
        const auto& pts = l.edges[e].points;
        CHECK_GE(pts.size(), std::size_t{2});
        if (pts.size() < 2) continue;
        const FlowNode& a = c.nodes[c.edges[e].from];
        const FlowNode& b = c.nodes[c.edges[e].to];
        CHECK(onBorder(a.shape, l.nodes[c.edges[e].from].rect, pts.front()));
        CHECK(onBorder(b.shape, l.nodes[c.edges[e].to].rect, pts.back()));
    }
}

void checkInsideCanvas(const FlowLayout& l) {
    for (const auto& n : l.nodes) CHECK(contains(Rect(0, 0, l.width, l.height), n.rect));
    for (const auto& e : l.edges) {
        for (const Point& p : e.points) {
            CHECK(p.x >= -0.01 && p.x <= l.width + 0.01 && p.y >= -0.01 && p.y <= l.height + 0.01);
        }
    }
}

template <typename T> std::size_t countItems(const DiagramScene& s) {
    return static_cast<std::size_t>(
        std::count_if(s.items.begin(), s.items.end(), [](const DiagramItem& i) { return std::holds_alternative<T>(i); }));
}

std::vector<std::string> sceneTexts(const DiagramScene& s) {
    std::vector<std::string> out;
    for (const DiagramItem& i : s.items) {
        if (const auto* t = std::get_if<DiagramText>(&i)) out.push_back(t->utf8);
    }
    return out;
}

bool hasText(const DiagramScene& s, std::string_view text) {
    const auto texts = sceneTexts(s);
    return std::find(texts.begin(), texts.end(), text) != texts.end();
}

} // namespace

// ------------------------------------------------------------- parser: flow

RIVET_TEST(mermaidFenceInfo) {
    CHECK(isMermaidFenceInfo("mermaid"));
    CHECK(isMermaidFenceInfo("  Mermaid "));
    CHECK(isMermaidFenceInfo("mermaid {theme: dark}"));
    CHECK(!isMermaidFenceInfo("mermaidx"));
    CHECK(!isMermaidFenceInfo("cpp"));
    CHECK(!isMermaidFenceInfo(""));
}

RIVET_TEST(mermaidFlowHeaderAndDirections) {
    CHECK(flow("graph TD\nA-->B").direction == FlowDirection::TopBottom);
    CHECK(flow("graph TB\nA-->B").direction == FlowDirection::TopBottom);
    CHECK(flow("flowchart BT\nA-->B").direction == FlowDirection::BottomTop);
    CHECK(flow("flowchart LR\nA-->B").direction == FlowDirection::LeftRight);
    CHECK(flow("graph RL\nA-->B").direction == FlowDirection::RightLeft);
    CHECK(flow("graph\nA-->B").direction == FlowDirection::TopBottom);
    // Statements on the header line, ';' separators, CRLF.
    const Flowchart c = flow("graph LR;A-->B;B-->C\r\n  C-->D;\r\n");
    CHECK_EQ(c.nodes.size(), std::size_t{4});
    CHECK_EQ(c.edges.size(), std::size_t{3});
    CHECK(!parseMermaid("graph XY\nA-->B"));
}

RIVET_TEST(mermaidFlowNodeShapes) {
    const Flowchart c = flow("flowchart TD\n"
                             "a[Rect] --> b(Round)\n"
                             "c([Stadium]) --> d{Diamond}\n"
                             "e((Circle)) --> f\n"
                             "g[[Sub]] --> h[(Db)]\n"
                             "i{{Hex}} --> j>Flag]\n"
                             "k[\"Quoted [label]\"] --> l[/Para/]\n");
    const auto shape = [&](std::string_view id) { return c.nodes[nodeIndex(c, id)].shape; };
    const auto label = [&](std::string_view id) { return c.nodes[nodeIndex(c, id)].label; };
    CHECK(shape("a") == NodeShape::Rect);
    CHECK(shape("b") == NodeShape::Round);
    CHECK(shape("c") == NodeShape::Stadium);
    CHECK(shape("d") == NodeShape::Diamond);
    CHECK(shape("e") == NodeShape::Circle);
    CHECK(shape("f") == NodeShape::Rect);
    CHECK(shape("g") == NodeShape::Rect);
    CHECK(shape("h") == NodeShape::Rect);
    CHECK(shape("i") == NodeShape::Diamond);
    CHECK(shape("j") == NodeShape::Rect);
    CHECK(label("a") == std::vector<std::string>{"Rect"});
    CHECK(label("e") == std::vector<std::string>{"Circle"});
    CHECK(label("f") == std::vector<std::string>{"f"}); // id is the default label
    CHECK(label("h") == std::vector<std::string>{"Db"});
    CHECK(label("k") == std::vector<std::string>{"Quoted [label]"});
    CHECK(label("l") == std::vector<std::string>{"Para"});
}

RIVET_TEST(mermaidFlowLabelsLineBreaksAndEntities) {
    const Flowchart c = flow("graph TD\nA[One<br>Two<br/>Three<BR />Four] --> B[a #quot;q#quot; &amp; b]");
    CHECK(c.nodes[0].label == (std::vector<std::string>{"One", "Two", "Three", "Four"}));
    CHECK(c.nodes[1].label == std::vector<std::string>{"a \"q\" & b"});
}

RIVET_TEST(mermaidFlowEdgeKinds) {
    const Flowchart c = flow("graph LR\n"
                             "A --> B\n"
                             "A --- C\n"
                             "A -.-> D\n"
                             "A -.- E\n"
                             "A ==> F\n"
                             "A === G\n"
                             "A ----> H\n"
                             "A -..-> I\n"
                             "A ====> J\n"
                             "A --x K\n");
    CHECK_EQ(c.edges.size(), std::size_t{10});
    const auto check = [&](std::size_t i, EdgeStroke stroke, bool arrow) {
        CHECK(c.edges[i].stroke == stroke);
        CHECK_EQ(c.edges[i].arrow, arrow);
        CHECK(c.edges[i].label.empty());
    };
    check(0, EdgeStroke::Solid, true);
    check(1, EdgeStroke::Solid, false);
    check(2, EdgeStroke::Dotted, true);
    check(3, EdgeStroke::Dotted, false);
    check(4, EdgeStroke::Thick, true);
    check(5, EdgeStroke::Thick, false);
    check(6, EdgeStroke::Solid, true);
    check(7, EdgeStroke::Dotted, true);
    check(8, EdgeStroke::Thick, true);
    check(9, EdgeStroke::Solid, true);
}

RIVET_TEST(mermaidFlowEdgeLabels) {
    const Flowchart c = flow("graph TD\n"
                             "A -->|yes| B\n"
                             "A -- no way --> C\n"
                             "A -. maybe .-> D\n"
                             "A == sure ==> E\n"
                             "A ---|plain| F\n"
                             "A -- line --- G\n"
                             "A -->|\"quoted|x\"| H\n");
    CHECK(c.edges[0].label == std::vector<std::string>{"yes"});
    CHECK(c.edges[1].label == std::vector<std::string>{"no way"});
    CHECK(c.edges[1].stroke == EdgeStroke::Solid && c.edges[1].arrow);
    CHECK(c.edges[2].label == std::vector<std::string>{"maybe"});
    CHECK(c.edges[2].stroke == EdgeStroke::Dotted && c.edges[2].arrow);
    CHECK(c.edges[3].label == std::vector<std::string>{"sure"});
    CHECK(c.edges[3].stroke == EdgeStroke::Thick && c.edges[3].arrow);
    CHECK(c.edges[4].label == std::vector<std::string>{"plain"});
    CHECK(!c.edges[4].arrow);
    CHECK(c.edges[5].label == std::vector<std::string>{"line"});
    CHECK(!c.edges[5].arrow);
    CHECK_EQ(c.edges.size(), std::size_t{7});
}

RIVET_TEST(mermaidFlowChainsGroupsAndRedefinition) {
    const Flowchart c = flow("graph TD\nA --> B --> C\nA & B --> D & E\nB[Bee]\nmy-node --> A");
    CHECK_EQ(c.nodes.size(), std::size_t{6});
    CHECK_EQ(c.edges.size(), std::size_t{2 + 4 + 1});
    CHECK(c.nodes[nodeIndex(c, "B")].label == std::vector<std::string>{"Bee"});
    CHECK_EQ(c.edges[0].from, nodeIndex(c, "A"));
    CHECK_EQ(c.edges[1].to, nodeIndex(c, "C"));
    CHECK_EQ(c.edges[2].from, nodeIndex(c, "A"));
    CHECK_EQ(c.edges[2].to, nodeIndex(c, "D"));
    CHECK_EQ(c.edges[5].from, nodeIndex(c, "B"));
    CHECK_EQ(c.edges[5].to, nodeIndex(c, "E"));
    CHECK_EQ(c.edges[6].from, nodeIndex(c, "my-node"));
}

RIVET_TEST(mermaidFlowSubgraphsCommentsAndIgnoredStatements) {
    const Flowchart c = flow("---\ntitle: x\n---\n"
                             "%%{init: {'theme':'dark'}}%%\n"
                             "flowchart TB\n"
                             "  %% a comment\n"
                             "  classDef red fill:#f00\n"
                             "  subgraph one [First group]\n"
                             "    direction LR\n"
                             "    a1 --> a2\n"
                             "  end\n"
                             "  subgraph Two\n"
                             "    b1\n"
                             "  end\n"
                             "  a2 --> b1 --> c1\n"
                             "  class a1 red\n"
                             "  style a2 fill:#0f0\n"
                             "  linkStyle 0 stroke:#f00\n"
                             "  click a1 callback\n");
    CHECK_EQ(c.subgraphs.size(), std::size_t{2});
    CHECK_EQ(c.subgraphs[0].id, std::string("one"));
    CHECK_EQ(c.subgraphs[0].title, std::string("First group"));
    CHECK_EQ(c.subgraphs[1].title, std::string("Two"));
    CHECK_EQ(c.nodes[nodeIndex(c, "a1")].subgraph, 0);
    CHECK_EQ(c.nodes[nodeIndex(c, "a2")].subgraph, 0);
    CHECK_EQ(c.nodes[nodeIndex(c, "b1")].subgraph, 1);
    CHECK_EQ(c.nodes[nodeIndex(c, "c1")].subgraph, -1);
    CHECK_EQ(c.edges.size(), std::size_t{3});
}

RIVET_TEST(mermaidParseErrors) {
    const auto fails = [](std::string_view src) {
        const auto r = parseMermaid(src);
        CHECK(!r.has_value());
        if (!r) CHECK(!r.error().empty());
        return !r.has_value();
    };
    fails("");
    fails("   \n%% only a comment\n");
    fails("pie title Pets\n\"Dogs\" : 386");
    fails("classDiagram\nA <|-- B");
    fails("gantt\ntitle x");
    fails("graph TD\nsubgraph a\nsubgraph b\nend\nend");   // nested
    fails("graph TD\nsubgraph a\nA-->B");                 // unclosed
    fails("graph TD\nend");                               // stray end
    fails("graph TD\nA[unterminated --> B");
    fails("graph TD\nA -- dangling label B");
    fails("graph TD\nA ->> B");
    fails("graph TD\nA --> ");
    fails("graph TD\nA -->|open label B");
    fails("sequenceDiagram\nA->>B: hi\nend");
    fails("sequenceDiagram\nloop forever\nA->>B: hi");
    fails("sequenceDiagram\nelse nope");
    fails("sequenceDiagram\nthis is not a statement");
    fails("sequenceDiagram\nNote sideways of A: x");
    fails("sequenceDiagram\nNote left of A,B: x");
    fails("sequenceDiagram extra");
    // Error reasons never echo document text.
    const auto r = parseMermaid("graph TD\nSECRET_TOKEN ->> other");
    CHECK(!r.has_value());
    if (!r) CHECK(r.error().find("SECRET") == std::string::npos);
}

RIVET_TEST(mermaidLimits) {
    MermaidLimits limits;
    limits.maxNodes = 3;
    CHECK(parseMermaid("graph TD\nA-->B-->C", limits).has_value());
    CHECK(!parseMermaid("graph TD\nA-->B-->C-->D", limits).has_value());
    limits = {};
    limits.maxEdges = 2;
    CHECK(parseMermaid("graph TD\nA-->B-->C", limits).has_value());
    CHECK(!parseMermaid("graph TD\nA-->B-->C-->A", limits).has_value());
    CHECK(!parseMermaid("sequenceDiagram\nA->>B: 1\nA->>B: 2\nA->>B: 3", limits).has_value());
    limits = {};
    limits.maxSourceBytes = 16;
    CHECK(!parseMermaid("graph TD\nAAAA-->BBBB", limits).has_value());
    // Default limits: 64 KiB of source, 500 nodes.
    std::string big = "graph TD\n";
    while (big.size() <= 64 * 1024) big += "%% padding padding padding padding padding padding\n";
    CHECK(!parseMermaid(big).has_value());
    std::string many = "graph TD\n";
    for (int i = 0; i < 501; ++i) many += "n" + std::to_string(i) + "\n";
    CHECK(!parseMermaid(many).has_value());
    limits = {};
    limits.maxBlockDepth = 2;
    CHECK(parseMermaid("sequenceDiagram\nloop a\nloop b\nA->>B: x\nend\nend", limits).has_value());
    CHECK(!parseMermaid("sequenceDiagram\nloop a\nloop b\nloop c\nA->>B: x\nend\nend\nend", limits).has_value());
}

// --------------------------------------------------------- parser: sequence

RIVET_TEST(mermaidSequenceParticipantsAndMessages) {
    const SequenceDiagram d = seq("sequenceDiagram\n"
                                  "  participant A as Alice\n"
                                  "  actor B as Bob the builder\n"
                                  "  participant C\n"
                                  "  A->B: solid open\n"
                                  "  A-->B: dashed open\n"
                                  "  A->>B: solid arrow\n"
                                  "  A-->>B: dashed arrow\n"
                                  "  A-xB: cross\n"
                                  "  A--xB: dashed cross\n"
                                  "  A-)B: async\n"
                                  "  A--)B: dashed async\n"
                                  "  B->>+C: activate\n"
                                  "  C-->>-B: deactivate\n"
                                  "  activate A\n"
                                  "  deactivate A\n"
                                  "  Web-Server->>D: hyphenated\n"
                                  "  D->>D: self\n"
                                  "  A->>B:\n");
    CHECK_EQ(d.participants.size(), std::size_t{5});
    CHECK_EQ(d.participants[0].label, std::string("Alice"));
    CHECK(!d.participants[0].actor);
    CHECK_EQ(d.participants[1].label, std::string("Bob the builder"));
    CHECK(d.participants[1].actor);
    CHECK_EQ(d.participants[3].id, std::string("Web-Server"));
    CHECK_EQ(d.events.size(), std::size_t{13});
    const auto msg = [&](std::size_t i) { return std::get<SeqMessage>(d.events[i]); };
    CHECK(!msg(0).dashed && msg(0).head == ArrowHead::None);
    CHECK(msg(1).dashed && msg(1).head == ArrowHead::None);
    CHECK(!msg(2).dashed && msg(2).head == ArrowHead::Filled);
    CHECK(msg(3).dashed && msg(3).head == ArrowHead::Filled);
    CHECK(!msg(4).dashed && msg(4).head == ArrowHead::Cross);
    CHECK(msg(5).dashed && msg(5).head == ArrowHead::Cross);
    CHECK(!msg(6).dashed && msg(6).head == ArrowHead::Open);
    CHECK(msg(7).dashed && msg(7).head == ArrowHead::Open);
    CHECK_EQ(msg(8).to, std::size_t{2});
    CHECK_EQ(msg(9).from, std::size_t{2});
    CHECK_EQ(msg(0).text, std::string("solid open"));
    CHECK_EQ(msg(11).from, msg(11).to);
    CHECK(msg(12).text.empty());
    CHECK_EQ(msg(0).number, std::size_t{0});
}

RIVET_TEST(mermaidSequenceNotesBlocksAutonumber) {
    const SequenceDiagram d = seq("sequenceDiagram\n"
                                  "autonumber\n"
                                  "A->>B: one\n"
                                  "Note left of A: left<br>two lines\n"
                                  "note right of B: right\n"
                                  "Note over A: over\n"
                                  "Note over B,A: wide\n"
                                  "loop Every minute\n"
                                  "  A->>B: two\n"
                                  "  alt ok\n"
                                  "    B-->>A: three\n"
                                  "  else failed\n"
                                  "    B-->>A: four\n"
                                  "  end\n"
                                  "  opt maybe\n"
                                  "  end\n"
                                  "end\n"
                                  "par first\nA->>B: five\nand second\nA->>B: six\nend\n"
                                  "critical c\nA->>B: seven\noption o\nA->>B: eight\nend\n"
                                  "break b\nA->>B: nine\nend\n"
                                  "rect rgb(0,0,0)\nA->>B: ten\nend\n");
    CHECK(d.autonumber);
    const auto& n0 = std::get<SeqNote>(d.events[1]);
    CHECK(n0.placement == NotePlacement::LeftOf);
    CHECK_EQ(n0.text, std::string("left\ntwo lines"));
    CHECK(std::get<SeqNote>(d.events[2]).placement == NotePlacement::RightOf);
    const auto& over2 = std::get<SeqNote>(d.events[4]);
    CHECK(over2.placement == NotePlacement::Over);
    CHECK_EQ(over2.first, std::size_t{0}); // normalized A..B
    CHECK_EQ(over2.last, std::size_t{1});
    CHECK(std::get<SeqBlockStart>(d.events[5]).kind == SeqBlockKind::Loop);
    CHECK_EQ(std::get<SeqBlockStart>(d.events[5]).label, std::string("Every minute"));
    CHECK(std::get<SeqBlockStart>(d.events[7]).kind == SeqBlockKind::Alt);
    CHECK_EQ(std::get<SeqBlockElse>(d.events[9]).label, std::string("failed"));
    std::vector<std::size_t> numbers;
    for (const SeqEvent& e : d.events) {
        if (const auto* m = std::get_if<SeqMessage>(&e)) numbers.push_back(m->number);
    }
    CHECK_EQ(numbers.size(), std::size_t{10});
    for (std::size_t i = 0; i < numbers.size(); ++i) CHECK_EQ(numbers[i], i + 1);
    std::size_t starts = 0;
    std::size_t ends = 0;
    for (const SeqEvent& e : d.events) {
        starts += std::holds_alternative<SeqBlockStart>(e) ? 1 : 0;
        ends += std::holds_alternative<SeqBlockEnd>(e) ? 1 : 0;
    }
    CHECK_EQ(starts, std::size_t{7});
    CHECK_EQ(ends, std::size_t{7});
}

// ---------------------------------------------------------- flowchart layout

RIVET_TEST(mermaidFlowRanksFollowDirection) {
    for (const auto& [header, dir] : std::vector<std::pair<std::string, FlowDirection>>{
             {"graph TD", FlowDirection::TopBottom},
             {"graph BT", FlowDirection::BottomTop},
             {"graph LR", FlowDirection::LeftRight},
             {"graph RL", FlowDirection::RightLeft}}) {
        const Flowchart c = flow(header + "\nA-->B-->C");
        const FlowLayout l = layoutOf(c);
        CHECK_EQ(l.rankCount, std::size_t{3});
        CHECK_EQ(l.nodes[0].rank, std::size_t{0});
        CHECK_EQ(l.nodes[1].rank, std::size_t{1});
        CHECK_EQ(l.nodes[2].rank, std::size_t{2});
        const Point a = center(l.nodes[0].rect);
        const Point b = center(l.nodes[1].rect);
        const Point cc = center(l.nodes[2].rect);
        switch (dir) {
        case FlowDirection::TopBottom:
            CHECK(a.y < b.y && b.y < cc.y);
            CHECK_NEAR(a.x, cc.x, 0.01); // a straight chain stays aligned
            break;
        case FlowDirection::BottomTop: CHECK(a.y > b.y && b.y > cc.y); break;
        case FlowDirection::LeftRight:
            CHECK(a.x < b.x && b.x < cc.x);
            CHECK_NEAR(a.y, cc.y, 0.01);
            break;
        case FlowDirection::RightLeft: CHECK(a.x > b.x && b.x > cc.x); break;
        }
        checkNoNodeOverlap(l);
        checkEndpointsOnBorders(c, l);
        checkInsideCanvas(l);
    }
}

RIVET_TEST(mermaidFlowLongestPathRanking) {
    // D is reachable in one step from A and in three via B, C: longest path wins.
    const Flowchart c = flow("graph TD\nA-->B-->C-->D\nA-->D\nX-->D");
    const FlowLayout l = layoutOf(c);
    CHECK_EQ(l.nodes[nodeIndex(c, "D")].rank, std::size_t{3});
    // The source X is pulled down next to its only successor.
    CHECK_EQ(l.nodes[nodeIndex(c, "X")].rank, std::size_t{2});
    // The long edge A-->D runs through two dummies.
    CHECK_EQ(l.edges[3].points.size(), std::size_t{4});
    // Its dummies do not sit on top of B or C.
    for (std::size_t k = 1; k + 1 < l.edges[3].points.size(); ++k) {
        const Point p = l.edges[3].points[k];
        for (const auto& n : l.nodes) CHECK(!(p.x > n.rect.minX() && p.x < n.rect.maxX() && p.y > n.rect.minY() && p.y < n.rect.maxY()));
    }
    checkNoNodeOverlap(l);
    checkEndpointsOnBorders(c, l);
}

RIVET_TEST(mermaidFlowCrossingReduction) {
    // Declared so that the initial order crosses; the ordering removes it.
    const Flowchart planar = flow("graph TD\nX\nY\nA-->Y\nB-->X");
    const FlowLayout lp = layoutOf(planar);
    CHECK_EQ(lp.crossings, std::size_t{0});
    // A larger planar graph: two independent trees declared interleaved.
    const Flowchart trees = flow("graph TD\nr1-->a1\nr2-->b1\nr1-->a2\nr2-->b2\na1-->a3\nb1-->b3\na2-->a4\nb2-->b4");
    CHECK_EQ(layoutOf(trees).crossings, std::size_t{0});
    // K(2,2) has exactly one unavoidable crossing.
    const Flowchart k22 = flow("graph TD\nA-->C\nA-->D\nB-->C\nB-->D");
    CHECK_EQ(layoutOf(k22).crossings, std::size_t{1});
    // K(3,3) between two ranks: the minimum is 9 (Zarankiewicz).
    const Flowchart k33 = flow("graph TD\nA & B & C --> D & E & F");
    CHECK_EQ(layoutOf(k33).crossings, std::size_t{9});
}

RIVET_TEST(mermaidFlowCyclesAndSelfLoops) {
    const Flowchart c = flow("graph TD\nA-->B-->C-->A\nB-->B");
    const FlowLayout l = layoutOf(c);
    checkNoNodeOverlap(l);
    // Every edge is drawn in its own direction from border to border.
    checkEndpointsOnBorders(c, l);
    // The back edge C-->A starts at C and ends at A.
    const auto& back = l.edges[2].points;
    CHECK(onBorder(NodeShape::Rect, l.nodes[2].rect, back.front()));
    CHECK(onBorder(NodeShape::Rect, l.nodes[0].rect, back.back()));
    // Self-loop: four points to the right of B, both ends on B's right side.
    const auto& loop = l.edges[3].points;
    CHECK_EQ(loop.size(), std::size_t{4});
    CHECK_NEAR(loop.front().x, l.nodes[1].rect.maxX(), 0.01);
    CHECK_NEAR(loop.back().x, l.nodes[1].rect.maxX(), 0.01);
    CHECK_GT(loop[1].x, l.nodes[1].rect.maxX());
    checkInsideCanvas(l);
}

RIVET_TEST(mermaidFlowShapesBorderClipping) {
    const Flowchart c = flow("graph LR\nA{Decide}-->B((Round))\nA-->C([Stadium])\nB-->D(Rounded)\nC-->D\nA-->D");
    const FlowLayout l = layoutOf(c);
    checkEndpointsOnBorders(c, l);
    checkNoNodeOverlap(l);
    // Diamonds and circles are square.
    CHECK_NEAR(l.nodes[0].rect.size.width, l.nodes[0].rect.size.height, 1e-9);
    CHECK_NEAR(l.nodes[1].rect.size.width, l.nodes[1].rect.size.height, 1e-9);
    // A stadium is wider than tall.
    CHECK_GT(l.nodes[2].rect.size.width, l.nodes[2].rect.size.height);
}

RIVET_TEST(mermaidFlowNodeSizeFollowsLabel) {
    const Flowchart c = flow("graph TD\nA[short]-->B[a much longer label here]\nB-->C[two<br>lines]");
    const FlowLayout l = layoutOf(c);
    const DiagramStyle style;
    // FixedMeasurer: 2/3 of the font size per code point.
    const double charW = style.fontSize * 2.0 / 3.0;
    CHECK_NEAR(l.nodes[1].rect.size.width, 24.0 * charW + 2.0 * style.nodePadX, 1e-6);
    CHECK_GT(l.nodes[1].rect.size.width, l.nodes[0].rect.size.width);
    CHECK_GT(l.nodes[2].rect.size.height, l.nodes[0].rect.size.height);
}

RIVET_TEST(mermaidFlowEdgeLabelsGetTheirOwnSlot) {
    const Flowchart c = flow("graph TD\nA-->|a long edge label|B\nA-->C\nC-->B");
    const FlowLayout l = layoutOf(c);
    CHECK(l.edges[0].hasLabel);
    CHECK(!l.edges[1].hasLabel);
    // A labelled edge spans two ranks; the label sits between them.
    CHECK_GE(l.nodes[1].rank, l.nodes[0].rank + 2);
    const Rect& label = l.edges[0].labelRect;
    CHECK_GT(label.size.width, 0.0);
    for (const auto& n : l.nodes) CHECK(!overlaps(label, n.rect));
    CHECK_GT(center(label).y, l.nodes[0].rect.maxY());
    CHECK_LT(center(label).y, l.nodes[1].rect.minY());
    // The label centre lies on the edge's polyline.
    bool onPath = false;
    for (const Point& p : l.edges[0].points) onPath = onPath || (std::abs(p.x - center(label).x) < 0.01 && std::abs(p.y - center(label).y) < 0.01);
    CHECK(onPath);
}

RIVET_TEST(mermaidFlowSubgraphBoxes) {
    const Flowchart c = flow("graph TD\n"
                             "subgraph S [Group]\n a1-->a2\n end\n"
                             "x-->a1\n"
                             "a2-->y\n"
                             "x-->z\n");
    const FlowLayout l = layoutOf(c);
    CHECK_EQ(l.clusters.size(), std::size_t{1});
    const Rect box = l.clusters[0].rect;
    CHECK(contains(box, l.nodes[nodeIndex(c, "a1")].rect));
    CHECK(contains(box, l.nodes[nodeIndex(c, "a2")].rect));
    for (const char* outside : {"x", "y", "z"}) CHECK(!overlaps(box, l.nodes[nodeIndex(c, outside)].rect));
    // Room for the title above the first member.
    CHECK_GT(l.nodes[nodeIndex(c, "a1")].rect.minY() - box.minY(), DiagramStyle{}.fontSize);
    checkNoNodeOverlap(l);
    checkEndpointsOnBorders(c, l);
    checkInsideCanvas(l);
}

RIVET_TEST(mermaidFlowSubgraphMembersStayContiguous) {
    const Flowchart c = flow("graph TD\n"
                             "r-->p\nr-->s1\nr-->q\nr-->s2\n"
                             "subgraph G\ns1\ns2\nend\n");
    const FlowLayout l = layoutOf(c);
    const std::size_t o1 = l.nodes[nodeIndex(c, "s1")].order;
    const std::size_t o2 = l.nodes[nodeIndex(c, "s2")].order;
    CHECK_EQ(std::max(o1, o2) - std::min(o1, o2), std::size_t{1});
    const Rect box = l.clusters[0].rect;
    for (const char* outside : {"r", "p", "q"}) CHECK(!overlaps(box, l.nodes[nodeIndex(c, outside)].rect));
}

RIVET_TEST(mermaidFlowLayoutIsDeterministicAndBounded) {
    // A pseudo-random DAG with cycles and long edges.
    std::string src = "graph LR\n";
    std::uint32_t state = 7;
    const auto next = [&](std::uint32_t mod) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % mod;
    };
    for (int i = 0; i < 160; ++i) {
        src += "n" + std::to_string(next(80)) + " --> n" + std::to_string(next(80)) + "\n";
    }
    const Flowchart c = flow(src);
    const FlowLayout a = layoutOf(c);
    const FlowLayout b = layoutOf(c);
    CHECK_EQ(a.crossings, b.crossings);
    CHECK_EQ(a.width, b.width);
    CHECK_EQ(a.height, b.height);
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        CHECK_EQ(a.nodes[i].rect.minX(), b.nodes[i].rect.minX());
        CHECK_EQ(a.nodes[i].rect.minY(), b.nodes[i].rect.minY());
    }
    checkNoNodeOverlap(a);
    checkEndpointsOnBorders(c, a);
    checkInsideCanvas(a);
}

RIVET_TEST(mermaidFlowLayoutLimits) {
    const Flowchart c = flow("graph TD\nA-->B-->C-->D-->E\nA-->E");
    FixedMeasurer m;
    MermaidLimits limits;
    limits.maxDummies = 2;
    CHECK(!layoutFlowchart(c, m, {}, limits).has_value());
    limits.maxDummies = 3;
    CHECK(layoutFlowchart(c, m, {}, limits).has_value());
    // 500 nodes in a chain lay out within the default limits.
    std::string src = "graph TD\n";
    for (int i = 0; i < 499; ++i) src += "n" + std::to_string(i) + "-->n" + std::to_string(i + 1) + "\n";
    const Flowchart big = flow(src);
    CHECK_EQ(big.nodes.size(), std::size_t{500});
    const auto l = layoutFlowchart(big, m);
    CHECK(l.has_value());
    if (l) CHECK_EQ(l->rankCount, std::size_t{500});
}

// ---------------------------------------------------------- sequence layout

RIVET_TEST(mermaidSequenceGeometry) {
    const SequenceDiagram d = seq("sequenceDiagram\n"
                                  "participant A as Alice\n"
                                  "participant B as Bob\n"
                                  "participant C\n"
                                  "A->>B: hello\n"
                                  "B-->>A: hi\n"
                                  "A->>C: a very long message label that needs room\n"
                                  "C->>C: think\n");
    const SequenceLayout l = layoutOf(d);
    CHECK_EQ(l.participants.size(), std::size_t{3});
    CHECK(l.participants[0].x < l.participants[1].x && l.participants[1].x < l.participants[2].x);
    for (std::size_t i = 0; i + 1 < 3; ++i) CHECK(!overlaps(l.participants[i].top, l.participants[i + 1].top));
    CHECK_EQ(l.messages.size(), std::size_t{4});
    // Messages run top to bottom in source order, below the boxes and above
    // the mirrored boxes.
    double lastY = l.participants[0].top.maxY();
    for (const auto& m : l.messages) {
        CHECK_GT(m.points.front().y, lastY);
        lastY = m.points.back().y;
        CHECK_LT(lastY, l.participants[0].bottom.minY());
    }
    // From sender to receiver.
    CHECK_NEAR(l.messages[0].points.front().x, l.participants[0].x, 1e-9);
    CHECK_NEAR(l.messages[0].points.back().x, l.participants[1].x, 1e-9);
    CHECK_NEAR(l.messages[1].points.front().x, l.participants[1].x, 1e-9);
    CHECK_NEAR(l.messages[1].points.back().x, l.participants[0].x, 1e-9);
    // Labels sit above their line, centred.
    const auto& m0 = l.messages[0];
    CHECK_LT(m0.labelRect.maxY(), m0.points.front().y);
    CHECK_NEAR(center(m0.labelRect).x, (m0.points.front().x + m0.points.back().x) / 2.0, 1e-9);
    // The long label fits between A and C.
    const auto& m2 = l.messages[2];
    CHECK_GT(l.participants[2].x - l.participants[0].x, m2.labelRect.size.width);
    // Self message: a loop right of C with the label beside it.
    const auto& self = l.messages[3];
    CHECK_EQ(self.points.size(), std::size_t{4});
    CHECK_GT(self.points[1].x, l.participants[2].x);
    CHECK_NEAR(self.points.back().x, l.participants[2].x, 1e-9);
    CHECK_GT(self.labelRect.minX(), self.points[1].x);
    CHECK_LE(self.labelRect.maxX(), l.width);
}

RIVET_TEST(mermaidSequenceNotesAndFrames) {
    const SequenceDiagram d = seq("sequenceDiagram\n"
                                  "Note left of A: on the left\n"
                                  "A->>B: one\n"
                                  "Note right of B: right\n"
                                  "Note over A,B: spanning both\n"
                                  "loop Every minute\n"
                                  "  A->>B: two\n"
                                  "  alt ok\n"
                                  "    B-->>A: three\n"
                                  "  else failed\n"
                                  "    B-->>A: four\n"
                                  "  end\n"
                                  "end\n");
    const SequenceLayout l = layoutOf(d);
    CHECK_EQ(l.notes.size(), std::size_t{3});
    const double ax = l.participants[0].x;
    const double bx = l.participants[1].x;
    CHECK_LT(l.notes[0].rect.maxX(), ax);
    CHECK_GE(l.notes[0].rect.minX(), DiagramStyle{}.margin - 1e-9); // shifted into view
    CHECK_GT(l.notes[1].rect.minX(), bx);
    CHECK_LT(l.notes[2].rect.minX(), ax);
    CHECK_GT(l.notes[2].rect.maxX(), bx);
    CHECK_EQ(l.frames.size(), std::size_t{2});
    const auto& loop = l.frames[0];
    const auto& alt = l.frames[1];
    CHECK_EQ(loop.depth, std::size_t{0});
    CHECK_EQ(alt.depth, std::size_t{1});
    CHECK(contains(loop.rect, alt.rect));
    CHECK_LT(alt.rect.size.width, loop.rect.size.width);
    CHECK_EQ(alt.dividers.size(), std::size_t{1});
    // Messages two..four are inside the loop; three above, four below the divider.
    const auto inside = [](const SeqFrameGeometry& f, const SeqMessageGeometry& m) {
        return m.points.front().y > f.rect.minY() && m.points.front().y < f.rect.maxY() &&
               std::min(m.points.front().x, m.points.back().x) > f.rect.minX() &&
               std::max(m.points.front().x, m.points.back().x) < f.rect.maxX();
    };
    CHECK(!inside(loop, l.messages[0]));
    CHECK(inside(loop, l.messages[1]));
    CHECK(inside(alt, l.messages[2]));
    CHECK(inside(alt, l.messages[3]));
    CHECK_LT(l.messages[2].points.front().y, alt.dividers[0]);
    CHECK_GT(l.messages[3].points.front().y, alt.dividers[0]);
    // Notes stack vertically with the messages.
    CHECK_LT(l.notes[0].rect.maxY(), l.messages[0].points.front().y);
    CHECK_GT(l.notes[1].rect.minY(), l.messages[0].points.front().y);
}

// ------------------------------------------------------------------- scenes

RIVET_TEST(mermaidFlowScene) {
    FixedMeasurer m;
    const auto scene = renderMermaid("graph TD\nA[Start]-->|go|B{Ok?}\nB-->C((Done))\nB-.-D\nsubgraph S [Box]\nC\nend", m);
    CHECK(scene.has_value());
    if (!scene) return;
    for (const char* t : {"Start", "go", "Ok?", "Done", "D", "Box"}) CHECK(hasText(*scene, t));
    // Arrowheads only on arrow edges (2 of 3); diamond + circle are paths.
    CHECK_EQ(countItems<DiagramPath>(*scene), std::size_t{2 + 2});
    CHECK_EQ(countItems<DiagramPolyline>(*scene), std::size_t{3});
    // The dotted edge.
    bool dotted = false;
    for (const DiagramItem& i : scene->items) {
        if (const auto* p = std::get_if<DiagramPolyline>(&i)) dotted = dotted || p->dash == DashStyle::Dotted;
    }
    CHECK(dotted);
    // Cluster box first (painted behind).
    CHECK(std::holds_alternative<DiagramRect>(scene->items.front()));
    if (const auto* r = std::get_if<DiagramRect>(&scene->items.front())) CHECK(r->fill == DiagramColor::ClusterFill);
    // Everything inside the canvas; text widths come from the measurer.
    for (const DiagramItem& i : scene->items) {
        if (const auto* t = std::get_if<DiagramText>(&i)) {
            CHECK(!t->utf8.empty());
            CHECK_NEAR(t->width, m.measure(t->utf8, diagramTextStyle(t->font, t->size)).width, 1e-9);
            CHECK(t->origin.x >= 0.0 && t->origin.x + t->width <= scene->width + 1e-6);
            CHECK(t->origin.y >= 0.0 && t->origin.y <= scene->height);
        }
    }
}

RIVET_TEST(mermaidSequenceScene) {
    FixedMeasurer m;
    const auto scene = renderMermaid("sequenceDiagram\nautonumber\nactor U as User\nparticipant S as Server\n"
                                     "U->>S: request\nS-->>U: response\nS-xU: fail\nU-)S: async\n"
                                     "Note over S: thinking\nopt retry\nU->>S: again\nend",
                                     m);
    CHECK(scene.has_value());
    if (!scene) return;
    for (const char* t : {"User", "Server", "request", "response", "thinking", "opt", "[retry]", "1", "5"}) {
        CHECK(hasText(*scene, t));
    }
    std::size_t lifelines = 0;
    std::size_t dashed = 0;
    for (const DiagramItem& i : scene->items) {
        if (const auto* p = std::get_if<DiagramPolyline>(&i)) {
            lifelines += p->color == DiagramColor::Lifeline ? 1 : 0;
            dashed += (p->dash == DashStyle::Dashed && p->color == DiagramColor::EdgeStroke) ? 1 : 0;
        }
    }
    CHECK_EQ(lifelines, std::size_t{2});
    CHECK_EQ(dashed, std::size_t{1});
    // Participant names appear twice (top and bottom boxes).
    const auto texts = sceneTexts(*scene);
    CHECK_EQ(std::count(texts.begin(), texts.end(), std::string("Server")), 2);
    CHECK_GT(scene->width, 0.0);
    CHECK_GT(scene->height, 0.0);
}

RIVET_TEST(mermaidRenderReportsErrors) {
    FixedMeasurer m;
    CHECK(!renderMermaid("journey\ntitle x", m).has_value());
    CHECK(!renderMermaid("graph TD\nA-->", m).has_value());
    MermaidLimits limits;
    limits.maxDummies = 1;
    CHECK(!renderMermaid("graph TD\nA-->B-->C-->D\nA-->D", m, {}, limits).has_value());
}
