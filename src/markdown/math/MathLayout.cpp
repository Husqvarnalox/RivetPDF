// SPDX-License-Identifier: MPL-2.0

// Box-and-glue layout of parsed TeX math (The TeXbook, appendix G,
// simplified): four styles, inter-atom spacing, scripts, fractions,
// radicals, operator limits, delimiters sized by content, arrays, accents.
// Boxes use y-down coordinates relative to their baseline.

#include "markdown/math/MathAst.hpp"
#include "markdown/math/MathScene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rivet::markdown::math {

namespace {

struct Item {
    bool isRule = false;
    std::string text;
    MathFont font = MathFont::Math;
    MathColor color = MathColor::Normal;
    double size = 0.0;
    double x = 0.0;
    double y = 0.0; // glyph: baseline; rule: top
    double w = 0.0;
    double h = 0.0; // rule only
};

struct Box {
    double width = 0.0;
    double height = 0.0;
    double depth = 0.0;
    std::vector<Item> items;
};

void place(Box& dst, const Box& src, double dx, double dy) {
    for (Item item : src.items) {
        item.x += dx;
        item.y += dy;
        dst.items.push_back(std::move(item));
    }
}

void addRule(Box& box, double x, double top, double w, double h) {
    Item r;
    r.isRule = true;
    r.x = x;
    r.y = top;
    r.w = w;
    r.h = h;
    box.items.push_back(std::move(r));
}

MathStyle supStyle(MathStyle s) {
    return (s == MathStyle::Display || s == MathStyle::Text) ? MathStyle::Script : MathStyle::ScriptScript;
}

MathStyle fracInner(MathStyle s) {
    switch (s) {
    case MathStyle::Display: return MathStyle::Text;
    case MathStyle::Text: return MathStyle::Script;
    case MathStyle::Script:
    case MathStyle::ScriptScript: return MathStyle::ScriptScript;
    }
    return MathStyle::ScriptScript;
}

bool tight(MathStyle s) { return s == MathStyle::Script || s == MathStyle::ScriptScript; }

// TeX's inter-atom spacing table (rows: left class, columns: right class).
// 1 thin, 2 medium, 3 thick; negative: only in display and text styles.
constexpr std::array<std::array<int, 8>, 8> kSpacing = {{
    //  Ord Op  Bin Rel Open Close Punct Inner
    {{0, 1, -2, -3, 0, 0, 0, -1}},       // Ord
    {{1, 1, 0, -3, 0, 0, 0, -1}},        // Op
    {{-2, -2, 0, 0, -2, 0, 0, -2}},      // Bin
    {{-3, -3, 0, 0, -3, 0, 0, -3}},      // Rel
    {{0, 0, 0, 0, 0, 0, 0, 0}},          // Open
    {{0, 1, -2, -3, 0, 0, 0, -1}},       // Close
    {{-1, -1, 0, -1, -1, -1, -1, -1}},   // Punct
    {{-1, 1, -2, -3, -1, 0, -1, -1}},    // Inner
}};

double muBetween(AtomClass left, AtomClass right, MathStyle style) {
    const int v = kSpacing[static_cast<std::size_t>(left)][static_cast<std::size_t>(right)];
    if (v == 0 || (v < 0 && tight(style))) return 0.0;
    switch (std::abs(v)) {
    case 1: return 3.0;
    case 2: return 4.0;
    default: return 5.0;
    }
}

bool isMathItalicSymbol(const MathNode& n) {
    // Mathematical italic letters live in the astral plane (4-byte UTF-8);
    // h is U+210E.
    return n.kind == MathNode::Kind::Symbol && n.amount <= 0.0 &&
           ((n.text.size() == 4 && static_cast<unsigned char>(n.text[0]) == 0xF0) || n.text == "\xE2\x84\x8E");
}

// A lone symbol, possibly wrapped in braces: \hat{x} and \hat x alike.
const MathNode* singleSymbol(const MathNode* n) {
    while (n != nullptr && n->kind == MathNode::Kind::Group && n->children.size() == 1) n = &n->children[0];
    return n != nullptr && n->kind == MathNode::Kind::Symbol ? n : nullptr;
}

class Layout {
public:
    Layout(const IMathMeasurer& measurer, const MathOptions& options)
        : m_(measurer), p_(options.params), base_(options.fontSize) {}

    Box hlist(const std::vector<MathNode>& list, MathStyle style, bool cramped,
              std::optional<AtomClass> leading = std::nullopt) {
        struct Entry {
            Box box;
            AtomClass cls;
            bool isSpace;
            MathStyle style;
        };
        std::vector<Entry> entries;
        entries.reserve(list.size());
        MathStyle current = style;
        for (const MathNode& n : list) {
            if (n.kind == MathNode::Kind::StyleSwitch) {
                current = n.style;
                continue;
            }
            AtomClass cls = n.cls;
            Box b = layoutNode(n, current, cramped, cls);
            entries.push_back({std::move(b), cls, n.kind == MathNode::Kind::Space, current});
        }
        // Binary operators in a non-binary position become ordinary (rules 5, 6).
        std::optional<std::size_t> prev;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].isSpace) continue;
            AtomClass& cls = entries[i].cls;
            if (cls == AtomClass::Bin) {
                const AtomClass before = prev ? entries[*prev].cls : leading.value_or(AtomClass::Open);
                if (!prev && !leading) {
                    cls = AtomClass::Ord;
                } else if (before == AtomClass::Bin || before == AtomClass::Op || before == AtomClass::Rel ||
                           before == AtomClass::Open || before == AtomClass::Punct) {
                    cls = AtomClass::Ord;
                }
            }
            if ((cls == AtomClass::Rel || cls == AtomClass::Close || cls == AtomClass::Punct) && prev &&
                entries[*prev].cls == AtomClass::Bin) {
                entries[*prev].cls = AtomClass::Ord;
            }
            prev = i;
        }
        if (prev && entries[*prev].cls == AtomClass::Bin) entries[*prev].cls = AtomClass::Ord;

        Box out;
        double x = 0.0;
        std::optional<AtomClass> last = leading;
        bool any = false;
        for (Entry& e : entries) {
            if (!e.isSpace && last) x += muBetween(*last, e.cls, e.style) * size(e.style) / 18.0;
            place(out, e.box, x, 0.0);
            x += e.box.width;
            if (!any) {
                out.height = e.box.height;
                out.depth = e.box.depth;
                any = true;
            } else {
                out.height = std::max(out.height, e.box.height);
                out.depth = std::max(out.depth, e.box.depth);
            }
            if (!e.isSpace) last = e.cls;
        }
        out.width = x;
        return out;
    }

    bool errors() const { return errors_; }

private:
    double size(MathStyle s) const {
        switch (s) {
        case MathStyle::Display:
        case MathStyle::Text: return base_;
        case MathStyle::Script: return base_ * p_.scriptScale;
        case MathStyle::ScriptScript: return base_ * p_.scriptScriptScale;
        }
        return base_;
    }

    Box glyph(const std::string& text, MathFont font, double sz, MathColor color = MathColor::Normal) {
        Box b;
        if (text.empty()) return b;
        const MathGlyphMetrics gm = m_.measure(text, font, sz);
        b.width = gm.width;
        b.height = gm.ascent;
        b.depth = gm.descent;
        Item it;
        it.text = text;
        it.font = font;
        it.color = color;
        it.size = sz;
        it.w = gm.width;
        b.items.push_back(std::move(it));
        return b;
    }

    // A glyph scaled to at least `target` total height (ascent + descent)
    // and centred on the math axis.
    Box centredGlyph(const std::string& text, double target, double sz, double minScale = 1.0) {
        Box b;
        if (text.empty()) {
            b.width = p_.nullDelimiterSpace * sz;
            return b;
        }
        const MathGlyphMetrics g0 = m_.measure(text, MathFont::Math, sz);
        const double total0 = g0.ascent + g0.descent;
        double scale = minScale;
        if (total0 > 0.0) scale = std::max(scale, target / total0);
        scale = std::clamp(scale, 1.0, std::max(1.0, p_.maxDelimiterScale));
        const double s = sz * scale;
        const MathGlyphMetrics g = m_.measure(text, MathFont::Math, s);
        const double shift = (g.ascent - g.descent) / 2.0 - p_.axisHeight * sz; // down
        Item it;
        it.text = text;
        it.size = s;
        it.w = g.width;
        it.y = shift;
        b.items.push_back(std::move(it));
        b.width = g.width;
        b.height = g.ascent - shift;
        b.depth = g.descent + shift;
        return b;
    }

    Box layoutNode(const MathNode& n, MathStyle style, bool cramped, AtomClass& cls) {
        const double sz = size(style);
        switch (n.kind) {
        case MathNode::Kind::Symbol:
            if (n.amount > 0.0) {
                const MathGlyphMetrics g0 = m_.measure(n.text, MathFont::Math, sz);
                return centredGlyph(n.text, (g0.ascent + g0.descent) * n.amount, sz, n.amount);
            }
            return glyph(n.text, n.font, sz);
        case MathNode::Kind::BigOp:
            cls = AtomClass::Op;
            return bigOp(n, style);
        case MathNode::Kind::Text: return glyph(n.text, n.font, sz);
        case MathNode::Kind::Error:
            errors_ = true;
            return glyph(n.text, MathFont::Text, sz, MathColor::Error);
        case MathNode::Kind::Group: cls = AtomClass::Ord; return hlist(n.children, style, cramped);
        case MathNode::Kind::Scripts: return scripts(n, style, cramped, cls);
        case MathNode::Kind::Frac: cls = AtomClass::Inner; return frac(n, style, cramped);
        case MathNode::Kind::Sqrt: cls = AtomClass::Ord; return sqrt(n, style);
        case MathNode::Kind::Delimited: {
            cls = AtomClass::Inner;
            AtomClass inner = AtomClass::Ord;
            const Box body = n.children.empty() ? Box{} : layoutNode(n.children[0], style, cramped, inner);
            return delimit(body, n.left, n.right, style);
        }
        case MathNode::Kind::Matrix: return matrix(n, style, cls);
        case MathNode::Kind::Accent: cls = AtomClass::Ord; return accent(n, style, cramped);
        case MathNode::Kind::Space: {
            Box b;
            b.width = n.amount * sz;
            return b;
        }
        case MathNode::Kind::StyleSwitch: return {};
        case MathNode::Kind::Phantom: {
            cls = AtomClass::Ord;
            AtomClass inner = AtomClass::Ord;
            Box b = n.children.empty() ? Box{} : layoutNode(n.children[0], style, cramped, inner);
            b.items.clear();
            return b;
        }
        }
        return {};
    }

    Box bigOp(const MathNode& n, MathStyle style) {
        const double sz = size(style);
        if (n.named) return glyph(n.text, MathFont::Math, sz);
        const double s = style == MathStyle::Display ? sz * p_.displayOperatorScale : sz;
        const MathGlyphMetrics g = m_.measure(n.text, MathFont::Math, s);
        Box b;
        const double shift = (g.ascent - g.descent) / 2.0 - p_.axisHeight * sz;
        Item it;
        it.text = n.text;
        it.size = s;
        it.w = g.width;
        it.y = shift;
        b.items.push_back(std::move(it));
        b.width = g.width;
        b.height = g.ascent - shift;
        b.depth = g.descent + shift;
        return b;
    }

    Box scripts(const MathNode& n, MathStyle style, bool cramped, AtomClass& cls) {
        const MathNode& baseNode = n.children[0];
        AtomClass baseCls = baseNode.cls;
        Box base = layoutNode(baseNode, style, cramped, baseCls);
        cls = baseCls;
        const MathStyle ss = supStyle(style);
        std::optional<Box> sup;
        std::optional<Box> sub;
        AtomClass ignored = AtomClass::Ord;
        if (n.sup >= 0) sup = layoutNode(n.children[static_cast<std::size_t>(n.sup)], ss, cramped, ignored);
        if (n.sub >= 0) sub = layoutNode(n.children[static_cast<std::size_t>(n.sub)], ss, true, ignored);

        const bool limits = baseNode.kind == MathNode::Kind::BigOp &&
                            (baseNode.limits == LimitsMode::Limits ||
                             (baseNode.limits == LimitsMode::Auto && baseNode.limitsInDisplay &&
                              style == MathStyle::Display));
        const double sz = size(style);
        if (limits) {
            Box out;
            double w = base.width;
            if (sup) w = std::max(w, sup->width);
            if (sub) w = std::max(w, sub->width);
            place(out, base, (w - base.width) / 2.0, 0.0);
            out.width = w;
            out.height = base.height;
            out.depth = base.depth;
            if (sup) {
                const double gap = std::max(p_.bigOpSpacing1 * sz, p_.bigOpSpacing3 * sz - sup->depth);
                const double shift = base.height + gap + sup->depth;
                place(out, *sup, (w - sup->width) / 2.0, -shift);
                out.height = shift + sup->height + p_.bigOpSpacing5 * sz;
            }
            if (sub) {
                const double gap = std::max(p_.bigOpSpacing2 * sz, p_.bigOpSpacing4 * sz - sub->height);
                const double shift = base.depth + gap + sub->height;
                place(out, *sub, (w - sub->width) / 2.0, shift);
                out.depth = shift + sub->depth + p_.bigOpSpacing5 * sz;
            }
            return out;
        }

        const bool isChar = baseNode.kind == MathNode::Kind::Symbol || baseNode.kind == MathNode::Kind::Text ||
                            (baseNode.kind == MathNode::Kind::BigOp && baseNode.named);
        const double supSize = size(ss);
        double u = isChar ? 0.0 : base.height - p_.supDrop * supSize;
        double v = isChar ? 0.0 : base.depth + p_.subDrop * supSize;
        const double theta = p_.ruleThickness * sz;
        const double xh = p_.xHeight * sz;
        Box out;
        place(out, base, 0.0, 0.0);
        out.height = base.height;
        out.depth = base.depth;
        double scriptX = base.width;
        // Italic letters: the superscript sits a little to the right.
        const double italicKern = isMathItalicSymbol(baseNode) ? 0.05 * sz : 0.0;
        if (sup) {
            const double shift = cramped ? p_.supShiftCramped
                                 : style == MathStyle::Display ? p_.supShiftDisplay
                                                               : p_.supShiftText;
            u = std::max({u, shift * sz, sup->depth + xh / 4.0});
        }
        if (sub && !sup) {
            v = std::max({v, p_.subShift * sz, sub->height - 0.8 * xh});
        } else if (sub && sup) {
            v = std::max(v, p_.subShiftWithSup * sz);
            const double gap = (u - sup->depth) - (sub->height - v);
            if (gap < 4.0 * theta) {
                v += 4.0 * theta - gap;
                const double psi = 0.8 * xh - (u - sup->depth);
                if (psi > 0.0) {
                    u += psi;
                    v -= psi;
                }
            }
        }
        double width = scriptX;
        if (sup) {
            place(out, *sup, scriptX + italicKern, -u);
            out.height = std::max(out.height, u + sup->height);
            width = std::max(width, scriptX + italicKern + sup->width);
        }
        if (sub) {
            place(out, *sub, scriptX, v);
            out.depth = std::max(out.depth, v + sub->depth);
            width = std::max(width, scriptX + sub->width);
        }
        out.width = width + p_.scriptSpace * sz;
        return out;
    }

    Box frac(const MathNode& n, MathStyle outer, bool cramped) {
        const MathStyle style = n.fracStyle >= 0 ? static_cast<MathStyle>(n.fracStyle) : outer;
        const double sz = size(style);
        const MathStyle inner = fracInner(style);
        AtomClass ignored = AtomClass::Ord;
        const Box num = n.children.empty() ? Box{} : layoutNode(n.children[0], inner, cramped, ignored);
        const Box den = n.children.size() < 2 ? Box{} : layoutNode(n.children[1], inner, true, ignored);
        const bool display = style == MathStyle::Display;
        const double theta = n.rule ? p_.ruleThickness * sz : 0.0;
        const double defaultRule = p_.ruleThickness * sz;
        double u = (display ? p_.numShiftDisplay : p_.numShiftText) * sz;
        double v = (display ? p_.denShiftDisplay : p_.denShiftText) * sz;
        const double a = p_.axisHeight * sz;
        if (n.rule) {
            const double phi = display ? 3.0 * theta : theta;
            const double numGap = (u - num.depth) - (a + theta / 2.0);
            if (numGap < phi) u += phi - numGap;
            const double denGap = (a - theta / 2.0) - (den.height - v);
            if (denGap < phi) v += phi - denGap;
        } else {
            const double phi = display ? 7.0 * defaultRule : 3.0 * defaultRule;
            const double psi = (u - num.depth) - (den.height - v);
            if (psi < phi) {
                u += (phi - psi) / 2.0;
                v += (phi - psi) / 2.0;
            }
        }
        const double pad = p_.nullDelimiterSpace * sz;
        const double inner_w = std::max(num.width, den.width);
        Box out;
        place(out, num, pad + (inner_w - num.width) / 2.0, -u);
        place(out, den, pad + (inner_w - den.width) / 2.0, v);
        if (n.rule) addRule(out, pad, -(a + theta / 2.0), inner_w, theta);
        out.width = inner_w + 2.0 * pad;
        out.height = u + num.height;
        out.depth = v + den.depth;
        return out;
    }

    Box sqrt(const MathNode& n, MathStyle style) {
        const double sz = size(style);
        AtomClass ignored = AtomClass::Ord;
        const Box body = n.children.empty() ? Box{} : layoutNode(n.children[0], style, true, ignored);
        const double theta = p_.ruleThickness * sz;
        const double phi = style == MathStyle::Display ? p_.xHeight * sz : theta;
        const double clearance = theta + phi / 4.0;
        const double ruleTop = body.height + clearance + theta; // above the baseline
        const double need = body.height + body.depth + clearance + theta;
        static const std::string kRadical = "\xE2\x88\x9A"; // U+221A
        const MathGlyphMetrics g0 = m_.measure(kRadical, MathFont::Math, sz);
        const double total0 = g0.ascent + g0.descent;
        double scale = total0 > 0.0 ? need / total0 : 1.0;
        scale = std::clamp(scale, 1.0, std::max(1.0, p_.maxDelimiterScale));
        const double s = sz * scale;
        const MathGlyphMetrics g = m_.measure(kRadical, MathFont::Math, s);
        const double glyphBaseline = -ruleTop + g.ascent; // down

        Box out;
        double x = 0.0;
        std::optional<Box> index;
        if (n.children.size() > 1) index = layoutNode(n.children[1], MathStyle::ScriptScript, false, ignored);
        if (index) x = std::max(0.0, index->width - 0.5 * g.width);
        Item rad;
        rad.text = kRadical;
        rad.size = s;
        rad.w = g.width;
        rad.x = x;
        rad.y = glyphBaseline;
        out.items.push_back(std::move(rad));
        const double bodyX = x + g.width;
        place(out, body, bodyX, 0.0);
        addRule(out, bodyX, -ruleTop, body.width + theta, theta);
        out.width = bodyX + body.width + theta;
        out.height = ruleTop + theta;
        out.depth = std::max(body.depth, glyphBaseline + g.descent);
        if (index) {
            const double raise = 0.6 * (out.height - out.depth);
            place(out, *index, 0.0, -raise);
            out.height = std::max(out.height, raise + index->height);
        }
        return out;
    }

    Box delimit(const Box& body, const std::string& left, const std::string& right, MathStyle style) {
        const double sz = size(style);
        const double a = p_.axisHeight * sz;
        const double delta = std::max(body.height - a, body.depth + a);
        const double target =
            std::max(2.0 * delta * p_.delimiterFactor, 2.0 * delta - p_.delimiterShortfall * sz);
        const Box l = centredGlyph(left, target, sz);
        const Box r = centredGlyph(right, target, sz);
        Box out;
        place(out, l, 0.0, 0.0);
        place(out, body, l.width, 0.0);
        place(out, r, l.width + body.width, 0.0);
        out.width = l.width + body.width + r.width;
        out.height = std::max({l.height, body.height, r.height});
        out.depth = std::max({l.depth, body.depth, r.depth});
        return out;
    }

    Box matrix(const MathNode& n, MathStyle style, AtomClass& cls) {
        const std::size_t rows = n.rows;
        const std::size_t cols = n.columns;
        const double sz = size(style);
        MathStyle cellStyle = MathStyle::Text;
        if (n.matrix == MatrixKind::Small) cellStyle = MathStyle::Script;
        if (n.matrix == MatrixKind::Aligned || n.matrix == MatrixKind::Gathered) cellStyle = MathStyle::Display;
        const double csz = size(cellStyle);
        std::vector<Box> cells;
        cells.reserve(rows * cols);
        for (std::size_t i = 0; i < rows * cols && i < n.children.size(); ++i) {
            const std::size_t c = i % cols;
            const MathNode& cell = n.children[i];
            const bool relLead = n.matrix == MatrixKind::Aligned && c % 2 == 1;
            cells.push_back(hlist(cell.children, cellStyle, false,
                                  relLead ? std::optional<AtomClass>(AtomClass::Ord) : std::nullopt));
        }
        cells.resize(rows * cols);
        std::vector<double> colW(cols, 0.0);
        std::vector<double> rowH(rows, 0.85 * csz);
        std::vector<double> rowD(rows, 0.35 * csz);
        for (std::size_t r = 0; r < rows; ++r) {
            for (std::size_t c = 0; c < cols; ++c) {
                const Box& b = cells[r * cols + c];
                colW[c] = std::max(colW[c], b.width);
                rowH[r] = std::max(rowH[r], b.height);
                rowD[r] = std::max(rowD[r], b.depth);
            }
        }
        const double rowGap =
            (n.matrix == MatrixKind::Aligned || n.matrix == MatrixKind::Gathered) ? 0.3 * csz : 0.0;
        const auto colSep = [&](std::size_t c) { // before column c (c >= 1)
            switch (n.matrix) {
            case MatrixKind::Aligned: return c % 2 == 1 ? 0.0 : 2.0 * csz;
            case MatrixKind::Small: return 0.5 * csz;
            default: return 1.0 * csz;
            }
        };
        std::vector<double> colX(cols, 0.0);
        double width = 0.0;
        for (std::size_t c = 0; c < cols; ++c) {
            if (c > 0) width += colSep(c);
            colX[c] = width;
            width += colW[c];
        }
        double total = 0.0;
        for (std::size_t r = 0; r < rows; ++r) total += rowH[r] + rowD[r] + (r > 0 ? rowGap : 0.0);
        const double top = total / 2.0 + p_.axisHeight * sz;
        Box array;
        double y = -top;
        for (std::size_t r = 0; r < rows; ++r) {
            if (r > 0) y += rowGap;
            const double baseline = y + rowH[r];
            for (std::size_t c = 0; c < cols; ++c) {
                const Box& b = cells[r * cols + c];
                double x = colX[c] + (colW[c] - b.width) / 2.0;
                if (n.matrix == MatrixKind::Cases) x = colX[c];
                if (n.matrix == MatrixKind::Aligned) x = c % 2 == 0 ? colX[c] + colW[c] - b.width : colX[c];
                place(array, b, x, baseline);
            }
            y = baseline + rowD[r];
        }
        array.width = width;
        array.height = top;
        array.depth = total - top;

        std::string left;
        std::string right;
        switch (n.matrix) {
        case MatrixKind::Paren: left = "("; right = ")"; break;
        case MatrixKind::Bracket: left = "["; right = "]"; break;
        case MatrixKind::Brace: left = "{"; right = "}"; break;
        case MatrixKind::Vert: left = right = "|"; break;
        case MatrixKind::DoubleVert: left = right = "\xE2\x80\x96"; break;
        case MatrixKind::Cases: left = "{"; break;
        case MatrixKind::Matrix:
        case MatrixKind::Small:
        case MatrixKind::Aligned:
        case MatrixKind::Gathered: cls = AtomClass::Ord; return array;
        }
        cls = AtomClass::Inner;
        if (n.matrix == MatrixKind::Cases) {
            Box padded = array;
            padded.items.clear();
            place(padded, array, 0.2 * sz, 0.0);
            padded.width += 0.2 * sz;
            return delimit(padded, left, right, style);
        }
        return delimit(array, left, right, style);
    }

    Box accent(const MathNode& n, MathStyle style, bool /*cramped*/) {
        const double sz = size(style);
        AtomClass ignored = AtomClass::Ord;
        const MathNode* child = n.children.empty() ? nullptr : &n.children[0];
        const Box body = child ? layoutNode(*child, style, true, ignored) : Box{};
        const double theta = p_.ruleThickness * sz;
        Box out;
        place(out, body, 0.0, 0.0);
        out.width = body.width;
        out.height = body.height;
        out.depth = body.depth;
        if (n.accent == AccentKind::Bar || n.accent == AccentKind::Overline) {
            const double top = body.height + 3.0 * theta + theta;
            const double inset = n.accent == AccentKind::Bar && singleSymbol(child) != nullptr
                                     ? 0.1 * body.width
                                     : 0.0;
            addRule(out, inset, -top, body.width - 2.0 * inset, theta);
            out.height = top + theta;
            return out;
        }
        if (n.accent == AccentKind::Underline) {
            const double top = body.depth + 3.0 * theta;
            addRule(out, 0.0, top, body.width, theta);
            out.depth = top + 2.0 * theta;
            return out;
        }
        std::string text;
        double s = sz;
        switch (n.accent) {
        case AccentKind::Hat: text = "^"; break;
        case AccentKind::Tilde: text = "~"; break;
        case AccentKind::Vec: text = "\xE2\x86\x92"; s = sz * 0.8; break; // U+2192
        case AccentKind::Dot: text = "."; break;
        case AccentKind::DDot: text = ".."; break;
        case AccentKind::Check: text = "\xE2\x88\xA8"; s = sz * 0.6; break; // U+2228
        case AccentKind::Bar:
        case AccentKind::Overline:
        case AccentKind::Underline: break;
        }
        MathGlyphMetrics g = m_.measure(text, MathFont::Math, s);
        // Wide bodies get wider (scaled) hats, tildes and arrows.
        const bool stretchable =
            n.accent == AccentKind::Hat || n.accent == AccentKind::Tilde || n.accent == AccentKind::Vec;
        if (stretchable && g.width > 0.0 && body.width > 1.5 * g.width) {
            s *= std::min(body.width / (1.1 * g.width), 2.5);
            g = m_.measure(text, MathFont::Math, s);
        }
        const double gap = 1.5 * theta;
        const double up = body.height + gap + g.descent; // baseline raise
        const MathNode* symbolChild = singleSymbol(child);
        const double skew = symbolChild && isMathItalicSymbol(*symbolChild) ? 0.08 * sz : 0.0;
        Item it;
        it.text = text;
        it.size = s;
        it.w = g.width;
        it.x = (body.width - g.width) / 2.0 + skew;
        it.y = -up;
        out.items.push_back(std::move(it));
        out.height = std::max(body.height, up + g.ascent);
        return out;
    }

    const IMathMeasurer& m_;
    const MathParams& p_;
    double base_;
    bool errors_ = false;
};

} // namespace

std::expected<MathScene, std::string> typesetMath(std::string_view tex, bool display, const IMathMeasurer& measurer,
                                                  const MathOptions& options) {
    auto parsed = parseMath(tex, options.limits);
    if (!parsed) return std::unexpected(parsed.error());
    if (!(options.fontSize > 0.0) || !std::isfinite(options.fontSize)) {
        return std::unexpected(std::string("invalid font size"));
    }
    Layout layout(measurer, options);
    const Box box = layout.hlist(parsed->list, display ? MathStyle::Display : MathStyle::Text, false);

    MathScene scene;
    scene.width = std::max(0.0, box.width);
    scene.ascent = std::max(0.0, box.height);
    scene.descent = std::max(0.0, box.depth);
    scene.hasErrors = parsed->hasErrors || layout.errors();
    for (const Item& it : box.items) {
        if (it.isRule) {
            if (it.w > 0.0 && it.h > 0.0) scene.rules.push_back({core::Rect(it.x, it.y, it.w, it.h), it.color});
            continue;
        }
        if (it.text.empty()) continue;
        // Merge with the previous run when it continues on the same baseline.
        if (!scene.runs.empty()) {
            MathGlyphRun& last = scene.runs.back();
            if (last.font == it.font && last.color == it.color && last.size == it.size &&
                last.origin.y == it.y && std::abs(last.origin.x + last.width - it.x) < 1e-9) {
                last.utf8 += it.text;
                last.width += it.w;
                continue;
            }
        }
        MathGlyphRun run;
        run.utf8 = it.text;
        run.origin = core::Point(it.x, it.y);
        run.width = it.w;
        run.size = it.size;
        run.font = it.font;
        run.color = it.color;
        scene.runs.push_back(std::move(run));
    }
    return scene;
}

} // namespace rivet::markdown::math
