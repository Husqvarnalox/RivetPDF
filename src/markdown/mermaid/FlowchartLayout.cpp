// SPDX-License-Identifier: MPL-2.0

#include "markdown/mermaid/FlowchartLayout.hpp"

#include "core/geometry/Size.hpp"
#include "markdown/mermaid/DiagramGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <tuple>
#include <utility>
#include <vector>

namespace rivet::markdown::mermaid {

namespace {

using detail::LabelBox;
using detail::measureLines;

constexpr std::size_t kOrderIterations = 24;
constexpr std::size_t kTransposePasses = 4;
constexpr std::size_t kCoordinateSweeps = 8;
constexpr double kLabelPad = 4.0;
constexpr double kSelfLoop = 16.0;

struct VNode {
    double su = 0.0; // extent along the order axis
    double sv = 0.0; // extent along the rank axis
    std::size_t rank = 0;
    int cluster = -1;
    bool dummy = false;
    double extraAfter = 0.0; // room on the order-axis "after" side (self-loops)
    std::vector<std::size_t> up;
    std::vector<std::size_t> down;
    double u = 0.0;
    double v = 0.0;
};

struct Graph {
    std::vector<VNode> nodes;
    std::vector<std::vector<std::size_t>> layers;
    std::vector<std::size_t> pos; // index within the layer
    std::size_t realCount = 0;

    void reindex() {
        for (const auto& layer : layers) {
            for (std::size_t i = 0; i < layer.size(); ++i) pos[layer[i]] = i;
        }
    }
};

// Inversions between two adjacent layers (Fenwick tree), O(E log V).
std::size_t crossingsBetween(const Graph& g, std::size_t r) {
    std::vector<std::pair<std::size_t, std::size_t>> segs;
    for (const std::size_t a : g.layers[r]) {
        for (const std::size_t b : g.nodes[a].down) segs.emplace_back(g.pos[a], g.pos[b]);
    }
    std::sort(segs.begin(), segs.end());
    const std::size_t width = g.layers[r + 1].size();
    std::vector<std::size_t> tree(width + 1, 0);
    std::size_t total = 0;
    std::size_t seen = 0;
    for (const auto& seg : segs) {
        // Count already-seen lower endpoints strictly greater than seg.second.
        std::size_t notGreater = 0;
        for (std::size_t i = seg.second + 1; i > 0; i -= i & (~i + 1)) notGreater += tree[i];
        total += seen - notGreater;
        for (std::size_t i = seg.second + 1; i <= width; i += i & (~i + 1)) ++tree[i];
        ++seen;
    }
    return total;
}

std::size_t totalCrossings(const Graph& g) {
    std::size_t total = 0;
    for (std::size_t r = 0; r + 1 < g.layers.size(); ++r) total += crossingsBetween(g, r);
    return total;
}

// Reorders layer r by the barycentre of its neighbours on one side, keeping
// cluster members contiguous.
void sortByBarycentre(Graph& g, std::size_t r, bool useUp) {
    std::vector<std::size_t>& layer = g.layers[r];
    std::vector<double> bary(layer.size());
    for (std::size_t i = 0; i < layer.size(); ++i) {
        const auto& nb = useUp ? g.nodes[layer[i]].up : g.nodes[layer[i]].down;
        if (nb.empty()) {
            bary[i] = static_cast<double>(i);
        } else {
            double sum = 0.0;
            for (const std::size_t n : nb) sum += static_cast<double>(g.pos[n]);
            bary[i] = sum / static_cast<double>(nb.size());
        }
    }
    std::vector<double> clusterSum;
    std::vector<double> clusterCount;
    for (std::size_t i = 0; i < layer.size(); ++i) {
        const int c = g.nodes[layer[i]].cluster;
        if (c < 0) continue;
        const auto ci = static_cast<std::size_t>(c);
        if (clusterSum.size() <= ci) {
            clusterSum.resize(ci + 1, 0.0);
            clusterCount.resize(ci + 1, 0.0);
        }
        clusterSum[ci] += bary[i];
        clusterCount[ci] += 1.0;
    }
    struct Key {
        double blockBary;
        std::size_t blockId;
        double bary;
        std::size_t index;
    };
    std::vector<Key> keys;
    keys.reserve(layer.size());
    for (std::size_t i = 0; i < layer.size(); ++i) {
        const int c = g.nodes[layer[i]].cluster;
        if (c >= 0) {
            const auto ci = static_cast<std::size_t>(c);
            keys.push_back({clusterSum[ci] / clusterCount[ci], ci, bary[i], i});
        } else {
            keys.push_back({bary[i], g.nodes.size() + layer[i], bary[i], i});
        }
    }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        return std::tie(a.blockBary, a.blockId, a.bary, a.index) < std::tie(b.blockBary, b.blockId, b.bary, b.index);
    });
    std::vector<std::size_t> sorted;
    sorted.reserve(layer.size());
    for (const Key& k : keys) sorted.push_back(layer[k.index]);
    layer = std::move(sorted);
    for (std::size_t i = 0; i < layer.size(); ++i) g.pos[layer[i]] = i;
}

// Crossings among the edges of x and y when x is left of y.
std::size_t pairCrossings(const Graph& g, std::size_t x, std::size_t y) {
    std::size_t c = 0;
    for (const std::size_t a : g.nodes[x].up) {
        for (const std::size_t b : g.nodes[y].up) c += g.pos[a] > g.pos[b] ? 1 : 0;
    }
    for (const std::size_t a : g.nodes[x].down) {
        for (const std::size_t b : g.nodes[y].down) c += g.pos[a] > g.pos[b] ? 1 : 0;
    }
    return c;
}

void transpose(Graph& g) {
    for (std::size_t pass = 0; pass < kTransposePasses; ++pass) {
        bool improved = false;
        for (auto& layer : g.layers) {
            for (std::size_t i = 0; i + 1 < layer.size(); ++i) {
                const std::size_t x = layer[i];
                const std::size_t y = layer[i + 1];
                if (g.nodes[x].cluster != g.nodes[y].cluster) continue;
                if (pairCrossings(g, y, x) < pairCrossings(g, x, y)) {
                    std::swap(layer[i], layer[i + 1]);
                    g.pos[x] = i + 1;
                    g.pos[y] = i;
                    improved = true;
                }
            }
        }
        if (!improved) break;
    }
}

// Weighted least squares placement of one layer: x_i as close as possible
// to desired_i with x_{i+1} - x_i >= sep_i (pool adjacent violators).
void placeLayer(Graph& g, const std::vector<std::size_t>& layer, const std::vector<double>& desired,
                const std::vector<double>& sep) {
    const std::size_t n = layer.size();
    if (n == 0) return;
    std::vector<double> offset(n, 0.0);
    for (std::size_t i = 1; i < n; ++i) offset[i] = offset[i - 1] + sep[i];
    struct Block {
        double sum;
        double weight;
        std::size_t count;
    };
    std::vector<Block> blocks;
    for (std::size_t i = 0; i < n; ++i) {
        blocks.push_back({desired[i] - offset[i], 1.0, 1});
        while (blocks.size() >= 2) {
            const Block& last = blocks.back();
            const Block& prev = blocks[blocks.size() - 2];
            if (prev.sum / prev.weight <= last.sum / last.weight) break;
            const Block merged{prev.sum + last.sum, prev.weight + last.weight, prev.count + last.count};
            blocks.pop_back();
            blocks.back() = merged;
        }
    }
    std::size_t i = 0;
    for (const Block& b : blocks) {
        const double y = b.sum / b.weight;
        for (std::size_t k = 0; k < b.count; ++k, ++i) g.nodes[layer[i]].u = y + offset[i];
    }
}

double meanU(const Graph& g, const std::vector<std::size_t>& nb, double fallback) {
    if (nb.empty()) return fallback;
    double sum = 0.0;
    for (const std::size_t n : nb) sum += g.nodes[n].u;
    return sum / static_cast<double>(nb.size());
}

core::Point borderPoint(NodeShape shape, const core::Rect& rect, core::Point toward) {
    const core::Point c(rect.minX() + rect.size.width / 2.0, rect.minY() + rect.size.height / 2.0);
    double dx = toward.x - c.x;
    double dy = toward.y - c.y;
    if (std::hypot(dx, dy) < 1e-9) {
        dx = 0.0;
        dy = 1.0;
    }
    const double hw = std::max(rect.size.width / 2.0, 1e-9);
    const double hh = std::max(rect.size.height / 2.0, 1e-9);
    double t = 0.0;
    switch (shape) {
    case NodeShape::Diamond:
        t = 1.0 / (std::abs(dx) / hw + std::abs(dy) / hh);
        break;
    case NodeShape::Circle:
        t = 1.0 / std::sqrt((dx / hw) * (dx / hw) + (dy / hh) * (dy / hh));
        break;
    case NodeShape::Rect:
    case NodeShape::Round:
    case NodeShape::Stadium:
        t = std::min(std::abs(dx) > 1e-12 ? hw / std::abs(dx) : std::numeric_limits<double>::infinity(),
                     std::abs(dy) > 1e-12 ? hh / std::abs(dy) : std::numeric_limits<double>::infinity());
        break;
    }
    return core::Point(c.x + dx * t, c.y + dy * t);
}

struct Bounds {
    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    void add(core::Point p) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }
    void add(const core::Rect& r) {
        add(r.origin);
        add(core::Point(r.maxX(), r.maxY()));
    }
};

core::Rect shifted(const core::Rect& r, double dx, double dy) {
    return core::Rect(r.minX() + dx, r.minY() + dy, r.size.width, r.size.height);
}

} // namespace

std::expected<FlowLayout, std::string> layoutFlowchart(const Flowchart& chart, const ITextMeasurer& measurer,
                                                       const DiagramStyle& style, const MermaidLimits& limits) {
    const std::size_t n = chart.nodes.size();
    const std::size_t m = chart.edges.size();
    if (n == 0) return std::unexpected(std::string("empty diagram"));
    if (n > limits.maxNodes) return std::unexpected(std::string("too many nodes"));
    if (m > limits.maxEdges) return std::unexpected(std::string("too many edges"));
    for (const FlowEdge& e : chart.edges) {
        if (e.from >= n || e.to >= n) return std::unexpected(std::string("invalid edge"));
    }
    const bool vertical =
        chart.direction == FlowDirection::TopBottom || chart.direction == FlowDirection::BottomTop;
    const std::size_t clusterCount = chart.subgraphs.size();

    // 1. Node sizes.
    std::vector<core::Size> size(n);
    for (std::size_t i = 0; i < n; ++i) {
        const LabelBox lb = measureLines(chart.nodes[i].label, measurer, DiagramFont::Regular, style);
        double w = lb.width + 2.0 * style.nodePadX;
        double h = lb.height + 2.0 * style.nodePadY;
        switch (chart.nodes[i].shape) {
        case NodeShape::Rect:
        case NodeShape::Round:
            break;
        case NodeShape::Stadium:
            w += h / 2.0;
            break;
        case NodeShape::Diamond: {
            const double side = lb.width + lb.height + 2.0 * style.nodePadY;
            w = h = std::max(side, h * 1.5);
            break;
        }
        case NodeShape::Circle: {
            const double d = std::hypot(lb.width, lb.height) + 2.0 * style.nodePadY;
            w = h = d;
            break;
        }
        }
        size[i] = core::Size(std::max(w, h), h);
        if (chart.nodes[i].shape == NodeShape::Diamond || chart.nodes[i].shape == NodeShape::Circle) {
            size[i] = core::Size(w, h);
        }
    }
    std::vector<LabelBox> edgeLabel(m);
    for (std::size_t e = 0; e < m; ++e) {
        if (!chart.edges[e].label.empty()) {
            edgeLabel[e] = measureLines(chart.edges[e].label, measurer, DiagramFont::Regular, style);
        }
    }

    // 2. Break cycles: DFS back edges are reversed.
    std::vector<std::vector<std::size_t>> out(n);
    for (std::size_t e = 0; e < m; ++e) {
        if (chart.edges[e].from != chart.edges[e].to) out[chart.edges[e].from].push_back(e);
    }
    std::vector<bool> reversed(m, false);
    {
        std::vector<int> state(n, 0); // 0 new, 1 on stack, 2 done
        std::vector<std::pair<std::size_t, std::size_t>> stack;
        for (std::size_t root = 0; root < n; ++root) {
            if (state[root] != 0) continue;
            stack.emplace_back(root, 0);
            state[root] = 1;
            while (!stack.empty()) {
                auto& [v, next] = stack.back();
                if (next < out[v].size()) {
                    const std::size_t e = out[v][next++];
                    const std::size_t t = chart.edges[e].to;
                    if (state[t] == 1) {
                        reversed[e] = true;
                    } else if (state[t] == 0) {
                        state[t] = 1;
                        stack.emplace_back(t, 0);
                    }
                } else {
                    state[v] = 2;
                    stack.pop_back();
                }
            }
        }
    }

    // 3. Longest-path ranking on the DAG.
    struct DagEdge {
        std::size_t a;
        std::size_t b;
        std::size_t minLen;
        std::size_t edge;
    };
    std::vector<DagEdge> dag;
    std::vector<std::vector<std::size_t>> dagOut(n);
    std::vector<std::vector<std::size_t>> dagIn(n);
    for (std::size_t e = 0; e < m; ++e) {
        const FlowEdge& fe = chart.edges[e];
        if (fe.from == fe.to) continue;
        const std::size_t a = reversed[e] ? fe.to : fe.from;
        const std::size_t b = reversed[e] ? fe.from : fe.to;
        dag.push_back({a, b, fe.label.empty() ? 1u : 2u, e});
        dagOut[a].push_back(dag.size() - 1);
        dagIn[b].push_back(dag.size() - 1);
    }
    std::vector<std::size_t> topo;
    topo.reserve(n);
    {
        std::vector<std::size_t> indeg(n, 0);
        for (const DagEdge& d : dag) ++indeg[d.b];
        for (std::size_t v = 0; v < n; ++v) {
            if (indeg[v] == 0) topo.push_back(v);
        }
        for (std::size_t head = 0; head < topo.size(); ++head) {
            for (const std::size_t di : dagOut[topo[head]]) {
                if (--indeg[dag[di].b] == 0) topo.push_back(dag[di].b);
            }
        }
        if (topo.size() != n) return std::unexpected(std::string("internal layout error"));
    }
    std::vector<std::size_t> rank(n, 0);
    for (const std::size_t v : topo) {
        for (const std::size_t di : dagOut[v]) rank[dag[di].b] = std::max(rank[dag[di].b], rank[v] + dag[di].minLen);
    }
    // Pull sources down next to their nearest successor.
    for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
        const std::size_t v = *it;
        if (!dagIn[v].empty() || dagOut[v].empty()) continue;
        std::size_t best = std::numeric_limits<std::size_t>::max();
        for (const std::size_t di : dagOut[v]) best = std::min(best, rank[dag[di].b] - dag[di].minLen);
        rank[v] = std::max(rank[v], best);
    }
    const std::size_t minRank = *std::min_element(rank.begin(), rank.end());
    for (std::size_t& r : rank) r -= minRank;
    const std::size_t rankCount = *std::max_element(rank.begin(), rank.end()) + 1;

    // 4. Virtual graph with dummies.
    Graph g;
    g.realCount = n;
    g.nodes.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        VNode& vn = g.nodes[i];
        vn.su = vertical ? size[i].width : size[i].height;
        vn.sv = vertical ? size[i].height : size[i].width;
        vn.rank = rank[i];
        const int sg = chart.nodes[i].subgraph;
        vn.cluster = (sg >= 0 && static_cast<std::size_t>(sg) < clusterCount) ? sg : -1;
    }
    std::vector<std::vector<std::size_t>> chain(m); // DAG direction, endpoints included
    std::vector<std::size_t> labelNode(m, std::numeric_limits<std::size_t>::max());
    for (const DagEdge& d : dag) {
        std::vector<std::size_t>& c = chain[d.edge];
        c.push_back(d.a);
        std::size_t prev = d.a;
        const int cl = g.nodes[d.a].cluster == g.nodes[d.b].cluster ? g.nodes[d.a].cluster : -1;
        for (std::size_t r = rank[d.a] + 1; r < rank[d.b]; ++r) {
            if (g.nodes.size() - n >= limits.maxDummies) return std::unexpected(std::string("diagram too large"));
            VNode dummy;
            dummy.dummy = true;
            dummy.rank = r;
            dummy.cluster = cl;
            g.nodes.push_back(std::move(dummy));
            const std::size_t id = g.nodes.size() - 1;
            g.nodes[prev].down.push_back(id);
            g.nodes[id].up.push_back(prev);
            c.push_back(id);
            prev = id;
        }
        g.nodes[prev].down.push_back(d.b);
        g.nodes[d.b].up.push_back(prev);
        c.push_back(d.b);
        if (!chart.edges[d.edge].label.empty() && c.size() >= 3) {
            const std::size_t li = c[(c.size() - 1) / 2];
            labelNode[d.edge] = li;
            const LabelBox& lb = edgeLabel[d.edge];
            const double lw = lb.width + 2.0 * kLabelPad;
            const double lh = lb.height + kLabelPad;
            g.nodes[li].su = vertical ? lw : lh;
            g.nodes[li].sv = vertical ? lh : lw;
        }
    }
    // Self-loops take room on the order axis "after" side.
    for (std::size_t e = 0; e < m; ++e) {
        const FlowEdge& fe = chart.edges[e];
        if (fe.from != fe.to) continue;
        double extra = kSelfLoop + style.arrowLength;
        if (!fe.label.empty()) {
            extra += kLabelPad + (vertical ? edgeLabel[e].width + 2.0 * kLabelPad : edgeLabel[e].height + kLabelPad);
        }
        g.nodes[fe.from].extraAfter = std::max(g.nodes[fe.from].extraAfter, extra);
    }

    // Layers, initial order by DFS discovery from the real nodes in source order.
    g.layers.assign(rankCount, {});
    g.pos.assign(g.nodes.size(), 0);
    {
        std::vector<bool> seen(g.nodes.size(), false);
        std::vector<std::size_t> stack;
        for (std::size_t root = 0; root < n; ++root) {
            if (seen[root]) continue;
            stack.push_back(root);
            while (!stack.empty()) {
                const std::size_t v = stack.back();
                stack.pop_back();
                if (seen[v]) continue;
                seen[v] = true;
                g.layers[g.nodes[v].rank].push_back(v);
                const auto& down = g.nodes[v].down;
                for (auto it = down.rbegin(); it != down.rend(); ++it) {
                    if (!seen[*it]) stack.push_back(*it);
                }
            }
        }
    }
    g.reindex();
    // Group cluster members from the start.
    for (std::size_t r = 0; r < rankCount; ++r) sortByBarycentre(g, r, true);

    // 5. Crossing reduction.
    std::vector<std::vector<std::size_t>> best = g.layers;
    std::size_t bestCrossings = totalCrossings(g);
    for (std::size_t it = 0; it < kOrderIterations && bestCrossings > 0; ++it) {
        if (it % 2 == 0) {
            for (std::size_t r = 1; r < rankCount; ++r) sortByBarycentre(g, r, true);
        } else {
            for (std::size_t r = rankCount - 1; r-- > 0;) sortByBarycentre(g, r, false);
        }
        transpose(g);
        const std::size_t c = totalCrossings(g);
        if (c < bestCrossings) {
            bestCrossings = c;
            best = g.layers;
        }
    }
    g.layers = std::move(best);
    g.reindex();

    // 6a. Order-axis coordinates.
    const double titleH = style.fontSize * style.lineHeightFactor;
    const auto separation = [&](std::size_t a, std::size_t b) {
        const VNode& na = g.nodes[a];
        const VNode& nb = g.nodes[b];
        double gap = 0.0;
        if (!na.dummy && !nb.dummy) {
            gap = style.nodeSep;
        } else if (na.dummy && nb.dummy) {
            gap = style.edgeSep;
        } else {
            gap = (style.nodeSep + style.edgeSep) / 2.0;
        }
        if (na.cluster != nb.cluster) {
            if (na.cluster >= 0) gap += style.clusterPad;
            if (nb.cluster >= 0) gap += style.clusterPad + (vertical ? 0.0 : titleH);
        }
        return (na.su + nb.su) / 2.0 + na.extraAfter + gap;
    };
    std::vector<std::vector<double>> seps(rankCount);
    for (std::size_t r = 0; r < rankCount; ++r) {
        const auto& layer = g.layers[r];
        seps[r].assign(layer.size(), 0.0);
        double x = 0.0;
        for (std::size_t i = 0; i < layer.size(); ++i) {
            if (i > 0) {
                seps[r][i] = separation(layer[i - 1], layer[i]);
                x += seps[r][i];
            }
            g.nodes[layer[i]].u = x;
        }
        for (const std::size_t v : layer) g.nodes[v].u -= x / 2.0;
    }
    const auto sweep = [&](std::size_t r, int mode) {
        const auto& layer = g.layers[r];
        std::vector<double> desired(layer.size());
        for (std::size_t i = 0; i < layer.size(); ++i) {
            const VNode& vn = g.nodes[layer[i]];
            if (mode == 0) {
                desired[i] = meanU(g, vn.up, vn.u);
            } else if (mode == 1) {
                desired[i] = meanU(g, vn.down, vn.u);
            } else {
                std::vector<std::size_t> both = vn.up;
                both.insert(both.end(), vn.down.begin(), vn.down.end());
                desired[i] = meanU(g, both, vn.u);
            }
        }
        placeLayer(g, layer, desired, seps[r]);
    };
    for (std::size_t k = 0; k < kCoordinateSweeps; ++k) {
        for (std::size_t r = 1; r < rankCount; ++r) sweep(r, 0);
        for (std::size_t r = rankCount - 1; r-- > 0;) sweep(r, 1);
    }
    for (std::size_t r = 0; r < rankCount; ++r) sweep(r, 2);

    // 6b. Rank-axis coordinates.
    std::vector<double> extent(rankCount, 0.0);
    std::vector<bool> onlyDummies(rankCount, true);
    for (std::size_t r = 0; r < rankCount; ++r) {
        for (const std::size_t v : g.layers[r]) {
            extent[r] = std::max(extent[r], g.nodes[v].sv);
            if (!g.nodes[v].dummy) onlyDummies[r] = false;
        }
    }
    std::vector<std::size_t> clusterMin(clusterCount, std::numeric_limits<std::size_t>::max());
    std::vector<std::size_t> clusterMax(clusterCount, 0);
    std::vector<bool> clusterUsed(clusterCount, false);
    for (const VNode& vn : g.nodes) {
        if (vn.cluster < 0) continue;
        const auto c = static_cast<std::size_t>(vn.cluster);
        clusterUsed[c] = true;
        clusterMin[c] = std::min(clusterMin[c], vn.rank);
        clusterMax[c] = std::max(clusterMax[c], vn.rank);
    }
    const bool titleAtLowRank = chart.direction == FlowDirection::TopBottom;
    const bool titleAtHighRank = chart.direction == FlowDirection::BottomTop;
    std::vector<double> rankPos(rankCount, 0.0);
    double clusterLead = 0.0; // room before rank 0 for cluster boxes starting there
    for (std::size_t c = 0; c < clusterCount; ++c) {
        if (!clusterUsed[c]) continue;
        if (clusterMin[c] == 0) clusterLead = std::max(clusterLead, style.clusterPad + (titleAtLowRank ? titleH : 0.0));
    }
    rankPos[0] = clusterLead + extent[0] / 2.0;
    for (std::size_t r = 0; r + 1 < rankCount; ++r) {
        double gap = (onlyDummies[r] || onlyDummies[r + 1]) ? style.rankSep / 2.0 : style.rankSep;
        double opening = 0.0;
        double closing = 0.0;
        for (std::size_t c = 0; c < clusterCount; ++c) {
            if (!clusterUsed[c]) continue;
            if (clusterMin[c] == r + 1) opening = std::max(opening, style.clusterPad + (titleAtLowRank ? titleH : 0.0));
            if (clusterMax[c] == r) closing = std::max(closing, style.clusterPad + (titleAtHighRank ? titleH : 0.0));
        }
        gap += opening + closing;
        rankPos[r + 1] = rankPos[r] + extent[r] / 2.0 + gap + extent[r + 1] / 2.0;
    }
    for (VNode& vn : g.nodes) vn.v = rankPos[vn.rank];

    // 7. Map to x/y.
    const auto centerOf = [&](std::size_t v) {
        const VNode& vn = g.nodes[v];
        double x = vertical ? vn.u : vn.v;
        double y = vertical ? vn.v : vn.u;
        if (chart.direction == FlowDirection::BottomTop) y = -y;
        if (chart.direction == FlowDirection::RightLeft) x = -x;
        return core::Point(x, y);
    };

    FlowLayout layout;
    layout.rankCount = rankCount;
    layout.crossings = totalCrossings(g);
    layout.nodes.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const core::Point c = centerOf(i);
        layout.nodes[i].rect =
            core::Rect(c.x - size[i].width / 2.0, c.y - size[i].height / 2.0, size[i].width, size[i].height);
        layout.nodes[i].rank = rank[i];
        layout.nodes[i].order = g.pos[i];
    }
    layout.edges.resize(m);
    for (std::size_t e = 0; e < m; ++e) {
        const FlowEdge& fe = chart.edges[e];
        FlowEdgePath& path = layout.edges[e];
        if (fe.from == fe.to) {
            const core::Rect& r = layout.nodes[fe.from].rect;
            const double cx = r.minX() + r.size.width / 2.0;
            const double cy = r.minY() + r.size.height / 2.0;
            const double reach = kSelfLoop + style.arrowLength;
            if (vertical) {
                const double x0 = borderPoint(chart.nodes[fe.from].shape, r, {r.maxX() + 1.0, cy - r.size.height / 4.0}).x;
                const double x1 = borderPoint(chart.nodes[fe.from].shape, r, {r.maxX() + 1.0, cy + r.size.height / 4.0}).x;
                const double y0 = cy - r.size.height / 4.0;
                const double y1 = cy + r.size.height / 4.0;
                const double xr = r.maxX() + reach;
                path.points = {{x0, y0}, {xr, y0}, {xr, y1}, {x1, y1}};
                if (!fe.label.empty()) {
                    const LabelBox& lb = edgeLabel[e];
                    path.hasLabel = true;
                    path.labelRect = core::Rect(xr + kLabelPad, cy - (lb.height + kLabelPad) / 2.0,
                                                lb.width + 2.0 * kLabelPad, lb.height + kLabelPad);
                }
            } else {
                const double x0 = cx - r.size.width / 4.0;
                const double x1 = cx + r.size.width / 4.0;
                const double y0 = borderPoint(chart.nodes[fe.from].shape, r, {x0, r.maxY() + 1.0}).y;
                const double y1 = borderPoint(chart.nodes[fe.from].shape, r, {x1, r.maxY() + 1.0}).y;
                const double yb = r.maxY() + reach;
                path.points = {{x0, y0}, {x0, yb}, {x1, yb}, {x1, y1}};
                if (!fe.label.empty()) {
                    const LabelBox& lb = edgeLabel[e];
                    path.hasLabel = true;
                    path.labelRect = core::Rect(cx - (lb.width + 2.0 * kLabelPad) / 2.0, yb + kLabelPad,
                                                lb.width + 2.0 * kLabelPad, lb.height + kLabelPad);
                }
            }
            continue;
        }
        std::vector<std::size_t> c = chain[e];
        std::vector<core::Point> pts;
        pts.reserve(c.size());
        for (const std::size_t v : c) pts.push_back(centerOf(v));
        if (reversed[e]) {
            std::reverse(pts.begin(), pts.end());
            std::reverse(c.begin(), c.end());
        }
        pts.front() = borderPoint(chart.nodes[fe.from].shape, layout.nodes[fe.from].rect, pts[1]);
        pts.back() = borderPoint(chart.nodes[fe.to].shape, layout.nodes[fe.to].rect, pts[pts.size() - 2]);
        path.points = std::move(pts);
        if (labelNode[e] != std::numeric_limits<std::size_t>::max()) {
            const LabelBox& lb = edgeLabel[e];
            const core::Point lc = centerOf(labelNode[e]);
            const double lw = lb.width + 2.0 * kLabelPad;
            const double lh = lb.height + kLabelPad;
            path.hasLabel = true;
            path.labelRect = core::Rect(lc.x - lw / 2.0, lc.y - lh / 2.0, lw, lh);
        }
    }

    // Cluster boxes.
    layout.clusters.resize(clusterCount);
    for (std::size_t c = 0; c < clusterCount; ++c) {
        Bounds b;
        for (std::size_t i = 0; i < n; ++i) {
            if (g.nodes[i].cluster == static_cast<int>(c)) b.add(layout.nodes[i].rect);
        }
        for (std::size_t v = n; v < g.nodes.size(); ++v) {
            if (g.nodes[v].cluster != static_cast<int>(c)) continue;
            const core::Point p = centerOf(v);
            const double w = vertical ? g.nodes[v].su : g.nodes[v].sv;
            const double h = vertical ? g.nodes[v].sv : g.nodes[v].su;
            b.add(core::Rect(p.x - w / 2.0, p.y - h / 2.0, w, h));
        }
        if (!clusterUsed[c] || !std::isfinite(b.minX)) {
            layout.clusters[c].rect = core::Rect();
            continue;
        }
        double minX = b.minX - style.clusterPad;
        double maxX = b.maxX + style.clusterPad;
        const double minY = b.minY - style.clusterPad - titleH;
        const double maxY = b.maxY + style.clusterPad;
        const TextStyle ts = diagramTextStyle(DiagramFont::Bold, style.fontSize);
        const double titleW =
            chart.subgraphs[c].title.empty() ? 0.0 : measurer.measure(chart.subgraphs[c].title, ts).width;
        if (maxX - minX < titleW + 2.0 * style.clusterPad) {
            const double grow = (titleW + 2.0 * style.clusterPad - (maxX - minX)) / 2.0;
            minX -= grow;
            maxX += grow;
        }
        layout.clusters[c].rect = core::Rect(minX, minY, maxX - minX, maxY - minY);
    }

    // Normalize to the margin.
    Bounds all;
    for (const FlowNodeBox& nb : layout.nodes) all.add(nb.rect);
    for (const FlowEdgePath& ep : layout.edges) {
        for (const core::Point& p : ep.points) all.add(p);
        if (ep.hasLabel) all.add(ep.labelRect);
    }
    for (std::size_t c = 0; c < clusterCount; ++c) {
        if (clusterUsed[c]) all.add(layout.clusters[c].rect);
    }
    const double dx = style.margin - all.minX;
    const double dy = style.margin - all.minY;
    for (FlowNodeBox& nb : layout.nodes) nb.rect = shifted(nb.rect, dx, dy);
    for (FlowEdgePath& ep : layout.edges) {
        for (core::Point& p : ep.points) p = core::Point(p.x + dx, p.y + dy);
        if (ep.hasLabel) ep.labelRect = shifted(ep.labelRect, dx, dy);
    }
    for (std::size_t c = 0; c < clusterCount; ++c) {
        if (clusterUsed[c]) layout.clusters[c].rect = shifted(layout.clusters[c].rect, dx, dy);
    }
    layout.width = all.maxX - all.minX + 2.0 * style.margin;
    layout.height = all.maxY - all.minY + 2.0 * style.margin;
    return layout;
}

DiagramScene flowchartScene(const Flowchart& chart, const FlowLayout& layout, const ITextMeasurer& measurer,
                            const DiagramStyle& style) {
    DiagramScene scene;
    scene.width = layout.width;
    scene.height = layout.height;
    const double titleH = style.fontSize * style.lineHeightFactor;

    // Subgraph boxes first.
    for (std::size_t c = 0; c < chart.subgraphs.size() && c < layout.clusters.size(); ++c) {
        const core::Rect& r = layout.clusters[c].rect;
        if (r.size.width <= 0.0 || r.size.height <= 0.0) continue;
        DiagramRect box;
        box.rect = r;
        box.cornerRadius = 4.0;
        box.fill = DiagramColor::ClusterFill;
        box.stroke = DiagramColor::ClusterStroke;
        box.strokeWidth = style.strokeWidth;
        scene.items.emplace_back(box);
        if (!chart.subgraphs[c].title.empty()) {
            const std::vector<std::string> lines{chart.subgraphs[c].title};
            const LabelBox lb = measureLines(lines, measurer, DiagramFont::Bold, style);
            detail::addCenteredText(scene, lines, lb,
                                    {r.minX() + r.size.width / 2.0, r.minY() + (titleH + style.clusterPad) / 2.0},
                                    DiagramFont::Bold, style.fontSize, DiagramColor::Text);
        }
    }

    // Edges.
    for (std::size_t e = 0; e < chart.edges.size() && e < layout.edges.size(); ++e) {
        const FlowEdge& fe = chart.edges[e];
        const FlowEdgePath& path = layout.edges[e];
        if (path.points.size() < 2) continue;
        DiagramPolyline line;
        line.points = path.points;
        line.width = fe.stroke == EdgeStroke::Thick ? style.thickStrokeWidth : style.strokeWidth;
        line.dash = fe.stroke == EdgeStroke::Dotted ? DashStyle::Dotted : DashStyle::Solid;
        if (fe.arrow) {
            const core::Point tip = path.points.back();
            const core::Point from = path.points[path.points.size() - 2];
            line.points.back() = detail::arrowBase(tip, from, style);
            scene.items.emplace_back(std::move(line));
            scene.items.emplace_back(detail::arrowHeadPath(tip, from, style));
        } else {
            scene.items.emplace_back(std::move(line));
        }
    }
    for (std::size_t e = 0; e < chart.edges.size() && e < layout.edges.size(); ++e) {
        const FlowEdgePath& path = layout.edges[e];
        if (!path.hasLabel) continue;
        DiagramRect bg;
        bg.rect = path.labelRect;
        bg.cornerRadius = 2.0;
        bg.fill = DiagramColor::LabelFill;
        bg.stroked = false;
        scene.items.emplace_back(bg);
        const LabelBox lb = measureLines(chart.edges[e].label, measurer, DiagramFont::Regular, style);
        detail::addCenteredText(scene, chart.edges[e].label, lb,
                                {path.labelRect.minX() + path.labelRect.size.width / 2.0,
                                 path.labelRect.minY() + path.labelRect.size.height / 2.0},
                                DiagramFont::Regular, style.fontSize, DiagramColor::Text);
    }

    // Nodes on top.
    for (std::size_t i = 0; i < chart.nodes.size() && i < layout.nodes.size(); ++i) {
        const FlowNode& node = chart.nodes[i];
        const core::Rect& r = layout.nodes[i].rect;
        const core::Point c(r.minX() + r.size.width / 2.0, r.minY() + r.size.height / 2.0);
        switch (node.shape) {
        case NodeShape::Rect:
        case NodeShape::Round:
        case NodeShape::Stadium: {
            DiagramRect box;
            box.rect = r;
            box.cornerRadius =
                node.shape == NodeShape::Rect ? 0.0 : node.shape == NodeShape::Round ? 6.0 : r.size.height / 2.0;
            box.strokeWidth = style.strokeWidth;
            scene.items.emplace_back(box);
            break;
        }
        case NodeShape::Diamond: {
            DiagramPath p;
            p.segments = detail::polygon({{c.x, r.minY()}, {r.maxX(), c.y}, {c.x, r.maxY()}, {r.minX(), c.y}});
            p.strokeWidth = style.strokeWidth;
            scene.items.emplace_back(std::move(p));
            break;
        }
        case NodeShape::Circle: {
            DiagramPath p;
            p.segments = detail::ellipse(r);
            p.strokeWidth = style.strokeWidth;
            scene.items.emplace_back(std::move(p));
            break;
        }
        }
        const LabelBox lb = measureLines(node.label, measurer, DiagramFont::Regular, style);
        detail::addCenteredText(scene, node.label, lb, c, DiagramFont::Regular, style.fontSize, DiagramColor::Text);
    }
    return scene;
}

} // namespace rivet::markdown::mermaid
