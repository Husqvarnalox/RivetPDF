// SPDX-License-Identifier: MPL-2.0
#include "editor/TextBlocks.hpp"

#include "editor/ContentGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>

namespace rivet::editor {
namespace {

using pdf::PdfContentObject;
using pdf::PdfContentObjectType;
using pdf::PdfPoint;

constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;

bool finite(double v) { return std::isfinite(v); }

bool finiteGeometry(const PdfContentObject& o) {
    const core::Matrix& m = o.matrix;
    return finite(m.a) && finite(m.b) && finite(m.c) && finite(m.d) && finite(m.tx) && finite(m.ty) &&
           finite(o.bounds.left) && finite(o.bounds.bottom) && finite(o.bounds.right) && finite(o.bounds.top) &&
           finite(o.fontSize);
}

bool quadIsEmpty(const std::array<PdfPoint, 4>& q) {
    return std::all_of(q.begin(), q.end(), [](const PdfPoint& p) { return p.x == 0.0 && p.y == 0.0; });
}

// The tight quad of an object, falling back to its axis-aligned bounds.
std::array<PdfPoint, 4> quadOf(const PdfContentObject& o) {
    if (!quadIsEmpty(o.quad)) return o.quad;
    return {PdfPoint{o.bounds.left, o.bounds.bottom}, PdfPoint{o.bounds.right, o.bounds.bottom},
            PdfPoint{o.bounds.right, o.bounds.top}, PdfPoint{o.bounds.left, o.bounds.top}};
}

struct Basis {
    double ux = 1.0;
    double uy = 0.0;
    // Unit normal pointing "up" in the text frame.
    double nx() const { return -uy; }
    double ny() const { return ux; }
};

Basis basisOf(const core::Matrix& m) {
    const double length = std::hypot(m.a, m.b);
    if (!(length > 0.0) || !finite(length)) return Basis{};
    return Basis{m.a / length, m.b / length};
}

double angleOf(const core::Matrix& m) { return std::atan2(m.b, m.a); }

double angleDifference(double a, double b) {
    double d = std::fmod(a - b, 2.0 * std::numbers::pi);
    if (d > std::numbers::pi) d -= 2.0 * std::numbers::pi;
    if (d < -std::numbers::pi) d += 2.0 * std::numbers::pi;
    return std::fabs(d);
}

// Whether an object may be grouped with others into lines / blocks (the
// per-object part of ADR-0015's "anything else stays a single-object block").
bool groupable(const PdfContentObject& o) {
    return o.type == PdfContentObjectType::Text && o.renderMode == 0 && !o.font.type3 && !o.textUnmappable &&
           !o.hasClip && o.fontSize > 0.0 && finiteGeometry(o) && geometry::isSimilarity(o.matrix);
}

struct ProjectionRange {
    double min = 0.0;
    double max = 0.0;
};

ProjectionRange project(const PdfContentObject& o, double dx, double dy) {
    ProjectionRange range{std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (const PdfPoint& p : quadOf(o)) {
        const double v = p.x * dx + p.y * dy;
        range.min = std::min(range.min, v);
        range.max = std::max(range.max, v);
    }
    return range;
}

bool startsWithSpace(const std::string& s) { return !s.empty() && (s.front() == ' ' || s.front() == '\n'); }
bool endsWithSpace(const std::string& s) { return !s.empty() && (s.back() == ' ' || s.back() == '\n'); }

struct LineInfo {
    std::vector<std::uint32_t> members;
    Basis basis;
    double angle = 0.0;
    double size = 0.0;
    PdfPoint origin; // first member's baseline origin
    double startProj = 0.0;
    double endProj = 0.0;     // furthest extent of any member along the baseline
    double lastEndProj = 0.0; // extent of the last member
    bool groupable = false;
    bool breakBefore = false; // a block edit's object was seen since the previous line
    std::string text;
};

LineInfo startLine(const pdf::PdfPageContent& content, std::uint32_t at) {
    const PdfContentObject& o = content.objects[at];
    LineInfo line;
    line.members.push_back(at);
    line.basis = basisOf(o.matrix);
    line.angle = angleOf(o.matrix);
    line.size = o.fontSize;
    line.origin = PdfPoint{o.matrix.tx, o.matrix.ty};
    line.startProj = o.matrix.tx * line.basis.ux + o.matrix.ty * line.basis.uy;
    const ProjectionRange range = project(o, line.basis.ux, line.basis.uy);
    line.endProj = std::max(line.startProj, range.max);
    line.lastEndProj = line.endProj;
    line.groupable = groupable(o);
    line.text = o.text;
    return line;
}

// ADR-0015: same rotation (0.5 deg), size (5 %), baseline (25 % of the size)
// and a horizontal gap in [-0.5, 1.5) x the size.
bool joinsLine(const LineInfo& line, const PdfContentObject& o, double& gapOut) {
    if (!line.groupable || !groupable(o)) return false;
    if (angleDifference(angleOf(o.matrix), line.angle) > kLineRotationToleranceDegrees * kRadiansPerDegree) {
        return false;
    }
    if (std::fabs(o.fontSize - line.size) > kLineSizeTolerance * std::max(o.fontSize, line.size)) return false;
    const double dx = o.matrix.tx - line.origin.x;
    const double dy = o.matrix.ty - line.origin.y;
    const double perpendicular = dx * line.basis.nx() + dy * line.basis.ny();
    if (std::fabs(perpendicular) > kLineBaselineTolerance * line.size) return false;
    const double gap = (o.matrix.tx * line.basis.ux + o.matrix.ty * line.basis.uy) - line.lastEndProj;
    if (!(gap < kLineMaxGap * line.size) || gap < -kLineMaxOverlap * line.size) return false;
    gapOut = gap;
    return true;
}

void appendToLine(LineInfo& line, std::uint32_t at, const PdfContentObject& o, double gap) {
    line.members.push_back(at);
    const ProjectionRange range = project(o, line.basis.ux, line.basis.uy);
    line.lastEndProj = std::max(range.max, o.matrix.tx * line.basis.ux + o.matrix.ty * line.basis.uy);
    line.endProj = std::max(line.endProj, line.lastEndProj);
    if (gap > kWordGapFactor * line.size && !endsWithSpace(line.text) && !startsWithSpace(o.text) && !o.text.empty() &&
        !line.text.empty()) {
        line.text += ' ';
    }
    line.text += o.text;
}

ReconstructedLine toReconstructedLine(const LineInfo& line) {
    ReconstructedLine out;
    out.members = line.members;
    out.text = line.text;
    out.baselineStart = line.origin;
    out.width = std::max(0.0, line.endProj - line.startProj);
    out.baselineEnd = PdfPoint{line.origin.x + line.basis.ux * out.width, line.origin.y + line.basis.uy * out.width};
    return out;
}

// ADR-0015: lines of one block share rotation and size, are left-aligned
// within 1 x size and step down by 0.8..2.2 x size (consistent within 25 %).
bool joinsBlock(const LineInfo& previous, const LineInfo& next, const LineInfo& blockFirst, double firstStep,
                double& stepOut) {
    if (next.breakBefore || !previous.groupable || !next.groupable) return false;
    if (angleDifference(next.angle, blockFirst.angle) > kLineRotationToleranceDegrees * kRadiansPerDegree) {
        return false;
    }
    if (std::fabs(next.size - blockFirst.size) > kLineSizeTolerance * std::max(next.size, blockFirst.size)) {
        return false;
    }
    const double dx = next.origin.x - previous.origin.x;
    const double dy = next.origin.y - previous.origin.y;
    const double along = dx * blockFirst.basis.ux + dy * blockFirst.basis.uy;
    if (std::fabs(along) > kBlockLeftTolerance * blockFirst.size) return false;
    const double step = -(dx * blockFirst.basis.nx() + dy * blockFirst.basis.ny());
    if (step < kBlockMinStep * blockFirst.size || step > kBlockMaxStep * blockFirst.size) return false;
    if (firstStep > 0.0 && std::fabs(step - firstStep) > kBlockStepConsistency * firstStep) return false;
    stepOut = step;
    return true;
}

ReconstructedBlock buildBlock(const pdf::PdfPageContent& content, const std::vector<ObjectCapabilityInfo>& capabilities,
                              const std::vector<ReconstructedLine>& lines, const LineInfo& first, std::uint64_t tag,
                              double measuredStep) {
    ReconstructedBlock block;
    block.lines = lines;
    block.tag = tag;
    block.angleRadians = first.angle;
    block.fontSize = first.size;
    block.lineAdvance = measuredStep > 0.0 ? measuredStep : kDefaultLineAdvanceFactor * first.size;

    double widest = 0.0;
    double minS = std::numeric_limits<double>::infinity(), maxS = -minS;
    double minT = minS, maxT = -minS;
    pdf::PdfBox bounds{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    bool haveCapability = false;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const ReconstructedLine& line = lines[li];
        if (li != 0) block.text += '\n';
        block.text += line.text;
        widest = std::max(widest, line.width);
        for (std::uint32_t member : line.members) {
            const PdfContentObject& o = content.objects[member];
            block.fontSubstituted = block.fontSubstituted || o.fontSubstituted;
            for (const PdfPoint& p : quadOf(o)) {
                const double s = p.x * first.basis.ux + p.y * first.basis.uy;
                const double t = p.x * first.basis.nx() + p.y * first.basis.ny();
                minS = std::min(minS, s);
                maxS = std::max(maxS, s);
                minT = std::min(minT, t);
                maxT = std::max(maxT, t);
            }
            bounds.left = std::min(bounds.left, o.bounds.left);
            bounds.bottom = std::min(bounds.bottom, o.bounds.bottom);
            bounds.right = std::max(bounds.right, o.bounds.right);
            bounds.top = std::max(bounds.top, o.bounds.top);
            const ObjectCapabilityInfo& info = capabilities[member];
            if (!haveCapability || info.capability < block.capability) {
                block.capability = info.capability;
                block.capabilityReason = info.reason;
                haveCapability = true;
            }
        }
    }
    block.bounds = bounds;
    const auto corner = [&](double s, double t) {
        return PdfPoint{first.basis.ux * s + first.basis.nx() * t, first.basis.uy * s + first.basis.ny() * t};
    };
    block.frame = {corner(minS, minT), corner(maxS, minT), corner(maxS, maxT), corner(minS, maxT)};
    block.wrapWidth = lines.size() > 1 ? widest + kWrapWidthSlackFactor * first.size : 0.0;
    return block;
}

} // namespace

std::optional<std::string> pageReadOnlyReason(const pdf::PdfPageContent& content) {
    if (content.truncated) return std::string("the page has too many objects to edit safely");
    if (!content.regenerationSafe) {
        return content.regenerationIssue.empty()
                   ? std::string("the page content cannot be rewritten without losing information")
                   : "the page content cannot be rewritten safely: " + content.regenerationIssue;
    }
    return std::nullopt;
}

ObjectCapabilityInfo classifyObject(const PdfContentObject& o) {
    if (!finiteGeometry(o)) return {ContentCapability::ReadOnly, "the object has invalid geometry"};
    switch (o.type) {
    case PdfContentObjectType::Text: {
        if (o.renderMode == 3) return {ContentCapability::ReadOnly, "invisible text (for example an OCR layer)"};
        if (o.renderMode >= 4) return {ContentCapability::ReadOnly, "text used as a clipping path"};
        if (o.font.type3) return {ContentCapability::MoveOnly, "the text uses a Type 3 font"};
        if (o.textUnmappable) return {ContentCapability::MoveOnly, "the text has no Unicode mapping"};
        if (o.hasClip) return {ContentCapability::MoveOnly, "the text is clipped"};
        if (!geometry::isSimilarity(o.matrix)) {
            return {ContentCapability::MoveOnly, "the text is skewed, mirrored or stretched"};
        }
        if (o.fontSubstituted) {
            return {ContentCapability::Replaceable, "the text was set in a substitute font"};
        }
        if (o.font.standard14 || (o.font.embedded && !o.font.subset)) return {ContentCapability::FullyEditable, {}};
        return {ContentCapability::Replaceable,
                o.font.subset ? "the font is a subset; a substitute font may be used"
                              : "the font is not embedded; a substitute font may be used"};
    }
    case PdfContentObjectType::Image:
        return {ContentCapability::FullyEditable, {}};
    case PdfContentObjectType::Path:
        return {ContentCapability::MoveOnly, "vector path"};
    case PdfContentObjectType::Form:
        return {ContentCapability::MoveOnly, "form object"};
    case PdfContentObjectType::Shading:
        return {ContentCapability::MoveOnly, "shading"};
    case PdfContentObjectType::Unknown:
        break;
    }
    return {ContentCapability::ReadOnly, "unsupported object type"};
}

std::vector<ObjectCapabilityInfo> classifyObjects(const pdf::PdfPageContent& content) {
    std::vector<ObjectCapabilityInfo> out;
    out.reserve(content.objects.size());
    const std::optional<std::string> pageReason = pageReadOnlyReason(content);
    for (const PdfContentObject& o : content.objects) {
        if (pageReason.has_value()) {
            out.push_back({ContentCapability::ReadOnly, *pageReason});
        } else {
            out.push_back(classifyObject(o));
        }
    }
    return out;
}

std::vector<ReconstructedBlock> reconstructTextBlocks(const pdf::PdfPageContent& content,
                                                      const std::vector<ObjectCapabilityInfo>& capabilities) {
    std::vector<ReconstructedBlock> blocks;
    if (content.objects.empty() || capabilities.size() != content.objects.size()) return blocks;

    // Pass 1: lines of the untagged text, in content order; tagged objects
    // are collected per edit tag (authoritative grouping).
    std::vector<LineInfo> lines;
    std::map<std::uint64_t, std::vector<std::uint32_t>> tagged;
    bool open = false; // lines.back() can still take members
    bool sawTagged = false;
    for (std::uint32_t at = 0; at < content.objects.size(); ++at) {
        const PdfContentObject& o = content.objects[at];
        if (o.type != PdfContentObjectType::Text) continue;
        if (o.blockTag != 0) {
            tagged[o.blockTag].push_back(at);
            open = false;
            sawTagged = true;
            continue;
        }
        double gap = 0.0;
        if (open && joinsLine(lines.back(), o, gap)) {
            appendToLine(lines.back(), at, o, gap);
            continue;
        }
        LineInfo line = startLine(content, at);
        line.breakBefore = sawTagged;
        sawTagged = false;
        lines.push_back(std::move(line));
        open = true;
    }

    // Pass 2: blocks of the untagged lines.
    std::size_t i = 0;
    while (i < lines.size()) {
        std::vector<ReconstructedLine> blockLines{toReconstructedLine(lines[i])};
        const LineInfo& first = lines[i];
        double firstStep = 0.0;
        std::size_t j = i + 1;
        while (j < lines.size()) {
            double step = 0.0;
            if (!joinsBlock(lines[j - 1], lines[j], first, firstStep, step)) break;
            if (firstStep == 0.0) firstStep = step;
            blockLines.push_back(toReconstructedLine(lines[j]));
            ++j;
        }
        blocks.push_back(buildBlock(content, capabilities, blockLines, first, 0, firstStep));
        i = j;
    }

    // Tagged blocks: one line per object, in object order.
    for (const auto& [tag, members] : tagged) {
        std::vector<ReconstructedLine> blockLines;
        blockLines.reserve(members.size());
        const LineInfo first = startLine(content, members.front());
        double firstStep = 0.0;
        for (std::size_t k = 0; k < members.size(); ++k) {
            const LineInfo line = startLine(content, members[k]);
            blockLines.push_back(toReconstructedLine(line));
            if (k == 1) {
                const double dx = line.origin.x - first.origin.x;
                const double dy = line.origin.y - first.origin.y;
                const double step = -(dx * first.basis.nx() + dy * first.basis.ny());
                if (step > 0.0 && finite(step)) firstStep = step;
            }
        }
        blocks.push_back(buildBlock(content, capabilities, blockLines, first, tag, firstStep));
    }

    std::stable_sort(blocks.begin(), blocks.end(), [](const ReconstructedBlock& a, const ReconstructedBlock& b) {
        return a.firstMember() < b.firstMember();
    });
    return blocks;
}

} // namespace rivet::editor
