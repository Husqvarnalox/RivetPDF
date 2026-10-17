// SPDX-License-Identifier: MPL-2.0

#include "markdown/mermaid/SequenceLayout.hpp"

#include "markdown/mermaid/DiagramGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rivet::markdown::mermaid {

namespace {

using detail::LabelBox;
using detail::measureLines;
using detail::splitLines;

constexpr double kNotePad = 8.0;
constexpr double kLabelGap = 3.0;   // between a message label and its line
constexpr double kSelfLoopHeight = 18.0;
constexpr double kFramePad = 22.0;
constexpr double kFramePadStep = 7.0;
constexpr double kActorFigure = 34.0; // stick figure height
constexpr double kBadgeRadius = 8.0;

std::string_view blockName(SeqBlockKind kind) {
    switch (kind) {
    case SeqBlockKind::Loop: return "loop";
    case SeqBlockKind::Alt: return "alt";
    case SeqBlockKind::Opt: return "opt";
    case SeqBlockKind::Par: return "par";
    case SeqBlockKind::Critical: return "critical";
    case SeqBlockKind::Break: return "break";
    case SeqBlockKind::Rect: return "";
    }
    return "";
}

std::string bracketed(const std::string& label) { return label.empty() ? std::string() : "[" + label + "]"; }

struct Span {
    std::size_t from;
    std::size_t to;
    double need; // minimum x[to] - x[from]
};

} // namespace

std::expected<SequenceLayout, std::string> layoutSequence(const SequenceDiagram& d, const ITextMeasurer& measurer,
                                                          const DiagramStyle& style, const MermaidLimits& limits) {
    const std::size_t pc = d.participants.size();
    if (pc == 0) return std::unexpected(std::string("empty diagram"));
    if (pc > limits.maxNodes) return std::unexpected(std::string("too many participants"));
    if (d.events.size() > limits.maxEdges) return std::unexpected(std::string("too many messages"));

    // Participant boxes.
    std::vector<LabelBox> plabel(pc);
    std::vector<double> boxW(pc);
    double boxH = 0.0;
    for (std::size_t i = 0; i < pc; ++i) {
        plabel[i] = measureLines(splitLines(d.participants[i].label), measurer, DiagramFont::Regular, style);
        boxW[i] = std::max(plabel[i].width + 2.0 * style.nodePadX, 60.0);
        const double h = d.participants[i].actor ? kActorFigure + plabel[i].height + 4.0
                                                 : plabel[i].height + 2.0 * style.nodePadY;
        boxH = std::max(boxH, h);
    }

    // Event labels and column constraints.
    std::vector<LabelBox> elabel(d.events.size());
    std::vector<Span> spans;
    double leftExtra = 0.0;
    std::vector<double> rightRoom(pc, 0.0); // room needed right of a lifeline (self messages, right notes)
    for (std::size_t e = 0; e < d.events.size(); ++e) {
        const SeqEvent& ev = d.events[e];
        if (const auto* msg = std::get_if<SeqMessage>(&ev)) {
            if (msg->from >= pc || msg->to >= pc) return std::unexpected(std::string("invalid message"));
            elabel[e] = measureLines(splitLines(msg->text), measurer, DiagramFont::Regular, style);
            const double badge = msg->number > 0 ? 2.0 * kBadgeRadius + 4.0 : 0.0;
            if (msg->from == msg->to) {
                rightRoom[msg->from] =
                    std::max(rightRoom[msg->from], style.selfLoopWidth + 6.0 + elabel[e].width + badge);
            } else {
                spans.push_back({std::min(msg->from, msg->to), std::max(msg->from, msg->to),
                                 elabel[e].width + 2.0 * style.nodePadX + style.arrowLength + badge});
            }
        } else if (const auto* note = std::get_if<SeqNote>(&ev)) {
            if (note->first >= pc || note->last >= pc) return std::unexpected(std::string("invalid note"));
            elabel[e] = measureLines(splitLines(note->text), measurer, DiagramFont::Regular, style);
            const double nw = std::max(style.noteWidthMin, elabel[e].width + 2.0 * kNotePad);
            switch (note->placement) {
            case NotePlacement::LeftOf:
                if (note->first == 0) {
                    leftExtra = std::max(leftExtra, nw + 10.0 - boxW[0] / 2.0);
                } else {
                    spans.push_back({note->first - 1, note->first, nw + 20.0});
                }
                break;
            case NotePlacement::RightOf:
                rightRoom[note->first] = std::max(rightRoom[note->first], nw + 10.0);
                break;
            case NotePlacement::Over:
                if (note->first != note->last) spans.push_back({note->first, note->last, nw - 2.0 * kNotePad});
                break;
            }
        } else if (const auto* start = std::get_if<SeqBlockStart>(&ev)) {
            elabel[e] = measureLines({bracketed(start->label)}, measurer, DiagramFont::Regular, style);
        } else if (const auto* alt = std::get_if<SeqBlockElse>(&ev)) {
            elabel[e] = measureLines({bracketed(alt->label)}, measurer, DiagramFont::Regular, style);
        }
    }

    // Column positions: one pass, every constraint ends at a later column.
    std::vector<double> x(pc, 0.0);
    x[0] = style.margin + std::max(0.0, leftExtra) + boxW[0] / 2.0;
    for (std::size_t i = 1; i < pc; ++i) {
        double need = boxW[i - 1] / 2.0 + boxW[i] / 2.0 + style.actorGap;
        need = std::max(need, rightRoom[i - 1] + 10.0);
        x[i] = x[i - 1] + need;
        for (const Span& s : spans) {
            if (s.to == i) x[i] = std::max(x[i], x[s.from] + s.need);
        }
    }

    SequenceLayout layout;
    layout.participants.resize(pc);
    const double top = style.margin;
    const double lineH = style.fontSize * style.lineHeightFactor;
    double y = top + boxH + style.messageGap + 4.0;

    struct OpenFrame {
        std::size_t frame;
        double minX;
        double maxX;
    };
    std::vector<OpenFrame> open;
    const auto touch = [&](double a, double b) {
        for (OpenFrame& f : open) {
            f.minX = std::min(f.minX, a);
            f.maxX = std::max(f.maxX, b);
        }
    };
    double maxX = x[pc - 1] + boxW[pc - 1] / 2.0 + rightRoom[pc - 1];
    double minX = x[0] - boxW[0] / 2.0;

    for (std::size_t e = 0; e < d.events.size(); ++e) {
        const SeqEvent& ev = d.events[e];
        const LabelBox& lb = elabel[e];
        if (const auto* msg = std::get_if<SeqMessage>(&ev)) {
            SeqMessageGeometry g;
            g.event = e;
            const double xa = x[msg->from];
            if (msg->from == msg->to) {
                const double lineY = y + 2.0;
                const double xr = xa + style.selfLoopWidth;
                g.points = {{xa, lineY}, {xr, lineY}, {xr, lineY + kSelfLoopHeight}, {xa, lineY + kSelfLoopHeight}};
                if (!msg->text.empty()) {
                    g.labelRect = core::Rect(xr + 6.0, lineY + kSelfLoopHeight / 2.0 - lb.height / 2.0, lb.width,
                                             lb.height);
                }
                touch(xa, xr + 6.0 + lb.width);
                maxX = std::max(maxX, xr + 6.0 + lb.width);
                y = lineY + std::max(kSelfLoopHeight, lb.height) + style.messageGap;
            } else {
                const double xb = x[msg->to];
                const double labelH = msg->text.empty() ? 0.0 : lb.height + kLabelGap;
                const double lineY = y + std::max(labelH, msg->number > 0 ? kBadgeRadius : 0.0);
                g.points = {{xa, lineY}, {xb, lineY}};
                if (!msg->text.empty()) {
                    g.labelRect =
                        core::Rect((xa + xb) / 2.0 - lb.width / 2.0, lineY - kLabelGap - lb.height, lb.width, lb.height);
                }
                touch(std::min(xa, xb), std::max(xa, xb));
                y = lineY + style.messageGap;
            }
            layout.messages.push_back(std::move(g));
        } else if (const auto* note = std::get_if<SeqNote>(&ev)) {
            const double nw = std::max(style.noteWidthMin, lb.width + 2.0 * kNotePad);
            const double nh = lb.height + 2.0 * kNotePad * 0.75;
            double left = 0.0;
            double width = nw;
            switch (note->placement) {
            case NotePlacement::LeftOf: left = x[note->first] - 10.0 - nw; break;
            case NotePlacement::RightOf: left = x[note->first] + 10.0; break;
            case NotePlacement::Over:
                if (note->first == note->last) {
                    left = x[note->first] - nw / 2.0;
                } else {
                    const double a = x[note->first] - kNotePad * 2.0;
                    const double b = x[note->last] + kNotePad * 2.0;
                    width = std::max(nw, b - a);
                    left = (a + b) / 2.0 - width / 2.0;
                }
                break;
            }
            const core::Rect rect(left, y, width, nh);
            layout.notes.push_back({e, rect});
            touch(rect.minX(), rect.maxX());
            minX = std::min(minX, rect.minX());
            maxX = std::max(maxX, rect.maxX());
            y += nh + style.messageGap;
        } else if (std::holds_alternative<SeqBlockStart>(ev)) {
            SeqFrameGeometry f;
            f.event = e;
            f.depth = open.size();
            f.rect = core::Rect(0.0, y, 0.0, 0.0);
            layout.frames.push_back(std::move(f));
            open.push_back({layout.frames.size() - 1, std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity()});
            y += lineH + 8.0;
        } else if (std::holds_alternative<SeqBlockElse>(ev)) {
            if (open.empty()) return std::unexpected(std::string("'else' outside a block"));
            layout.frames[open.back().frame].dividers.push_back(y);
            y += lineH + 8.0;
        } else if (std::holds_alternative<SeqBlockEnd>(ev)) {
            if (open.empty()) return std::unexpected(std::string("'end' without a block"));
            const OpenFrame f = open.back();
            open.pop_back();
            SeqFrameGeometry& frame = layout.frames[f.frame];
            double a = f.minX;
            double b = f.maxX;
            if (!std::isfinite(a)) {
                a = x.front();
                b = x.back();
            }
            // Deeper frames sit inside their parents.
            const double pad = std::max(4.0, kFramePad - kFramePadStep * static_cast<double>(frame.depth));
            a -= pad;
            b += pad;
            // Room for the title tab and label.
            const auto& sev = std::get<SeqBlockStart>(d.events[frame.event]);
            const TextStyle bold = diagramTextStyle(DiagramFont::Bold, style.fontSize);
            const double tabW = measurer.measure(std::string(blockName(sev.kind)), bold).width + 16.0;
            const double titleW = tabW + elabel[frame.event].width + 12.0;
            if (b - a < titleW) b = a + titleW;
            const double bottom = y + 2.0;
            frame.rect = core::Rect(a, frame.rect.minY(), b - a, bottom - frame.rect.minY());
            minX = std::min(minX, a);
            maxX = std::max(maxX, b);
            // The parent spans this frame.
            touch(a, b);
            y = bottom + style.messageGap;
        }
    }
    if (!open.empty()) return std::unexpected(std::string("unclosed block"));

    const double lifelineEnd = y + 4.0;
    for (std::size_t i = 0; i < pc; ++i) {
        SeqParticipantBox& box = layout.participants[i];
        box.x = x[i];
        box.top = core::Rect(x[i] - boxW[i] / 2.0, top, boxW[i], boxH);
        box.bottom = core::Rect(x[i] - boxW[i] / 2.0, lifelineEnd, boxW[i], boxH);
    }
    // Shift right when notes or frames reach past the left margin.
    const double shift = std::max(0.0, style.margin - minX);
    if (shift > 0.0) {
        const auto shiftRect = [shift](core::Rect& r) { r = core::Rect(r.minX() + shift, r.minY(), r.size.width, r.size.height); };
        for (SeqParticipantBox& p : layout.participants) {
            p.x += shift;
            shiftRect(p.top);
            shiftRect(p.bottom);
        }
        for (SeqMessageGeometry& g : layout.messages) {
            for (core::Point& p : g.points) p.x += shift;
            if (g.labelRect.size.width > 0.0) shiftRect(g.labelRect);
        }
        for (SeqNoteGeometry& n : layout.notes) shiftRect(n.rect);
        for (SeqFrameGeometry& f : layout.frames) shiftRect(f.rect);
        maxX += shift;
    }
    layout.width = maxX + style.margin;
    layout.height = lifelineEnd + boxH + style.margin;
    return layout;
}

DiagramScene sequenceScene(const SequenceDiagram& d, const SequenceLayout& layout, const ITextMeasurer& measurer,
                           const DiagramStyle& style) {
    DiagramScene scene;
    scene.width = layout.width;
    scene.height = layout.height;
    const double lineH = style.fontSize * style.lineHeightFactor;

    // Frames behind everything.
    for (const SeqFrameGeometry& f : layout.frames) {
        DiagramRect box;
        box.rect = f.rect;
        box.filled = false;
        box.stroke = DiagramColor::FrameStroke;
        box.strokeWidth = style.strokeWidth;
        scene.items.emplace_back(box);
        const auto& start = std::get<SeqBlockStart>(d.events[f.event]);
        const std::string name(blockName(start.kind));
        double labelX = f.rect.minX() + 6.0;
        if (!name.empty()) {
            const std::vector<std::string> lines{name};
            const LabelBox lb = measureLines(lines, measurer, DiagramFont::Bold, style);
            const double tabW = lb.width + 16.0;
            DiagramPath tab;
            tab.segments = detail::polygon({{f.rect.minX(), f.rect.minY()},
                                            {f.rect.minX() + tabW, f.rect.minY()},
                                            {f.rect.minX() + tabW, f.rect.minY() + lineH * 0.7},
                                            {f.rect.minX() + tabW - 6.0, f.rect.minY() + lineH + 2.0},
                                            {f.rect.minX(), f.rect.minY() + lineH + 2.0}});
            tab.fill = DiagramColor::FrameFill;
            tab.stroke = DiagramColor::FrameStroke;
            tab.strokeWidth = style.strokeWidth;
            scene.items.emplace_back(std::move(tab));
            detail::addCenteredText(scene, lines, lb, {f.rect.minX() + tabW / 2.0 - 2.0, f.rect.minY() + (lineH + 2.0) / 2.0},
                                    DiagramFont::Bold, style.fontSize, DiagramColor::Text);
            labelX = f.rect.minX() + tabW + 6.0;
        }
        const std::string title = bracketed(start.label);
        if (!title.empty()) {
            const std::vector<std::string> lines{title};
            const LabelBox lb = measureLines(lines, measurer, DiagramFont::Regular, style);
            detail::addCenteredText(scene, lines, lb, {labelX + lb.width / 2.0, f.rect.minY() + (lineH + 2.0) / 2.0},
                                    DiagramFont::Regular, style.fontSize, DiagramColor::Text);
        }
        // Dividers: find the matching else events in order.
        std::size_t depth = 0;
        std::size_t divider = 0;
        for (std::size_t e = f.event + 1; e < d.events.size() && divider < f.dividers.size(); ++e) {
            const SeqEvent& ev = d.events[e];
            if (std::holds_alternative<SeqBlockStart>(ev)) {
                ++depth;
            } else if (std::holds_alternative<SeqBlockEnd>(ev)) {
                if (depth == 0) break;
                --depth;
            } else if (const auto* alt = std::get_if<SeqBlockElse>(&ev); alt && depth == 0) {
                const double dy = f.dividers[divider++];
                DiagramPolyline line;
                line.points = {{f.rect.minX(), dy}, {f.rect.maxX(), dy}};
                line.color = DiagramColor::FrameStroke;
                line.width = style.strokeWidth;
                line.dash = DashStyle::Dashed;
                scene.items.emplace_back(std::move(line));
                const std::string label = bracketed(alt->label);
                if (!label.empty()) {
                    const std::vector<std::string> lines{label};
                    const LabelBox lb = measureLines(lines, measurer, DiagramFont::Regular, style);
                    detail::addCenteredText(scene, lines, lb,
                                            {f.rect.minX() + f.rect.size.width / 2.0, dy + (lineH + 8.0) / 2.0},
                                            DiagramFont::Regular, style.fontSize, DiagramColor::Text);
                }
            }
        }
    }

    // Lifelines and participant boxes.
    for (std::size_t i = 0; i < layout.participants.size() && i < d.participants.size(); ++i) {
        const SeqParticipantBox& p = layout.participants[i];
        DiagramPolyline life;
        life.points = {{p.x, p.top.maxY()}, {p.x, p.bottom.minY()}};
        life.color = DiagramColor::Lifeline;
        life.width = 1.0;
        life.dash = DashStyle::Dashed;
        scene.items.emplace_back(std::move(life));
        const std::vector<std::string> lines = splitLines(d.participants[i].label);
        const LabelBox lb = measureLines(lines, measurer, DiagramFont::Regular, style);
        for (const core::Rect& r : {p.top, p.bottom}) {
            const double cx = r.minX() + r.size.width / 2.0;
            if (d.participants[i].actor) {
                // Stick figure above the name.
                const double fy = r.minY() + 2.0;
                const double head = 7.0;
                DiagramPath headPath;
                headPath.segments = detail::ellipse(core::Rect(cx - head, fy, 2.0 * head, 2.0 * head));
                headPath.strokeWidth = style.strokeWidth;
                scene.items.emplace_back(std::move(headPath));
                const double neck = fy + 2.0 * head;
                const double hip = neck + 10.0;
                const double foot = r.minY() + kActorFigure - 2.0;
                for (const auto& seg : std::vector<std::vector<core::Point>>{
                         {{cx, neck}, {cx, hip}},
                         {{cx - 10.0, neck + 4.0}, {cx + 10.0, neck + 4.0}},
                         {{cx - 8.0, foot}, {cx, hip}, {cx + 8.0, foot}}}) {
                    DiagramPolyline limb;
                    limb.points = seg;
                    limb.color = DiagramColor::NodeStroke;
                    limb.width = style.strokeWidth;
                    scene.items.emplace_back(std::move(limb));
                }
                detail::addCenteredText(scene, lines, lb, {cx, r.minY() + kActorFigure + 2.0 + lb.height / 2.0},
                                        DiagramFont::Regular, style.fontSize, DiagramColor::Text);
            } else {
                DiagramRect box;
                box.rect = r;
                box.cornerRadius = 3.0;
                box.strokeWidth = style.strokeWidth;
                scene.items.emplace_back(box);
                detail::addCenteredText(scene, lines, lb, {cx, r.minY() + r.size.height / 2.0}, DiagramFont::Regular,
                                        style.fontSize, DiagramColor::Text);
            }
        }
    }

    // Messages.
    for (const SeqMessageGeometry& g : layout.messages) {
        const auto& msg = std::get<SeqMessage>(d.events[g.event]);
        if (g.points.size() < 2) continue;
        const core::Point tip = g.points.back();
        const core::Point from = g.points[g.points.size() - 2];
        DiagramPolyline line;
        line.points = g.points;
        line.width = style.strokeWidth;
        line.dash = msg.dashed ? DashStyle::Dashed : DashStyle::Solid;
        if (msg.head == ArrowHead::Filled) line.points.back() = detail::arrowBase(tip, from, style);
        scene.items.emplace_back(std::move(line));
        switch (msg.head) {
        case ArrowHead::None: break;
        case ArrowHead::Filled: scene.items.emplace_back(detail::arrowHeadPath(tip, from, style)); break;
        case ArrowHead::Open: {
            const core::Point dir = detail::unitVector(from, tip);
            const core::Point nrm(-dir.y, dir.x);
            const core::Point base = detail::arrowBase(tip, from, style);
            const double hw = style.arrowWidth / 2.0;
            DiagramPolyline open;
            open.points = {{base.x + nrm.x * hw, base.y + nrm.y * hw}, tip, {base.x - nrm.x * hw, base.y - nrm.y * hw}};
            open.width = style.strokeWidth;
            scene.items.emplace_back(std::move(open));
            break;
        }
        case ArrowHead::Cross: {
            const double s = style.arrowWidth / 2.0;
            for (const double k : {1.0, -1.0}) {
                DiagramPolyline cross;
                cross.points = {{tip.x - s, tip.y - s * k}, {tip.x + s, tip.y + s * k}};
                cross.width = style.strokeWidth;
                scene.items.emplace_back(std::move(cross));
            }
            break;
        }
        }
        if (msg.number > 0) {
            const core::Point c = g.points.front();
            DiagramPath badge;
            badge.segments = detail::ellipse(
                core::Rect(c.x - kBadgeRadius, c.y - kBadgeRadius, 2.0 * kBadgeRadius, 2.0 * kBadgeRadius));
            badge.fill = DiagramColor::NumberFill;
            badge.stroked = false;
            scene.items.emplace_back(std::move(badge));
            const std::vector<std::string> lines{std::to_string(msg.number)};
            DiagramStyle small = style;
            small.fontSize = style.fontSize * 0.75;
            const LabelBox lb = measureLines(lines, measurer, DiagramFont::Bold, small);
            detail::addCenteredText(scene, lines, lb, c, DiagramFont::Bold, small.fontSize, DiagramColor::NumberText);
        }
        if (!msg.text.empty()) {
            const std::vector<std::string> lines = splitLines(msg.text);
            const LabelBox lb = measureLines(lines, measurer, DiagramFont::Regular, style);
            detail::addCenteredText(scene, lines, lb,
                                    {g.labelRect.minX() + g.labelRect.size.width / 2.0,
                                     g.labelRect.minY() + g.labelRect.size.height / 2.0},
                                    DiagramFont::Regular, style.fontSize, DiagramColor::Text);
        }
    }

    // Notes on top.
    for (const SeqNoteGeometry& n : layout.notes) {
        const auto& note = std::get<SeqNote>(d.events[n.event]);
        DiagramRect box;
        box.rect = n.rect;
        box.fill = DiagramColor::NoteFill;
        box.stroke = DiagramColor::NoteStroke;
        box.strokeWidth = style.strokeWidth;
        scene.items.emplace_back(box);
        const std::vector<std::string> lines = splitLines(note.text);
        const LabelBox lb = measureLines(lines, measurer, DiagramFont::Regular, style);
        detail::addCenteredText(scene, lines, lb,
                                {n.rect.minX() + n.rect.size.width / 2.0, n.rect.minY() + n.rect.size.height / 2.0},
                                DiagramFont::Regular, style.fontSize, DiagramColor::Text);
    }
    return scene;
}

} // namespace rivet::markdown::mermaid
