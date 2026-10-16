// SPDX-License-Identifier: MPL-2.0
#pragma once

// Parsed TeX math (ADR-0032): the tree parseMath() builds and the layout in
// MathLayout.cpp consumes. Exposed for tests; callers normally use
// typesetMath() (MathScene.hpp).

#include "markdown/math/MathScene.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::markdown::math {

// TeX atom classes (inter-atom spacing).
enum class AtomClass : std::uint8_t { Ord, Op, Bin, Rel, Open, Close, Punct, Inner };

enum class MathStyle : std::uint8_t { Display, Text, Script, ScriptScript };

enum class LimitsMode : std::uint8_t { Auto, Limits, NoLimits };

enum class AccentKind : std::uint8_t { Hat, Tilde, Bar, Vec, Dot, DDot, Check, Overline, Underline };

enum class MatrixKind : std::uint8_t {
    Matrix,   // plain, centred
    Paren,    // pmatrix
    Bracket,  // bmatrix
    Brace,    // Bmatrix
    Vert,     // vmatrix
    DoubleVert, // Vmatrix
    Small,    // smallmatrix
    Cases,    // { left, columns left-aligned
    Aligned,  // aligned/align/align*/split: right/left column pairs
    Gathered, // centred, display style
};

struct MathNode {
    enum class Kind : std::uint8_t {
        Symbol,    // text: glyph(s) in `font`; cls; scale > 0 for \big-style delimiters
        BigOp,     // text: operator glyph (or name when `named`); limits
        Text,      // text: literal run in `font` (Text/TextBold)
        Error,     // text: literal source shown in the error colour
        Group,     // children: an hlist ({...})
        Scripts,   // children[0] base; sub/sup index children or -1
        Frac,      // children[0] numerator, [1] denominator; rule; fracStyle
        Sqrt,      // children[0] body, [1] optional index
        Delimited, // children[0] body; left/right delimiter glyphs ("" = none)
        Matrix,    // children: cells row-major (Group each); columns; matrix kind
        Accent,    // children[0] body; accent kind
        Space,     // amount (em of the current size)
        StyleSwitch, // style for the rest of the enclosing list
        Phantom,   // children[0] laid out but not drawn
    };

    Kind kind = Kind::Symbol;
    AtomClass cls = AtomClass::Ord;
    std::string text;
    MathFont font = MathFont::Math;
    bool named = false;           // BigOp: a word like "lim"
    bool limitsInDisplay = false; // BigOp: limits under/over in display style (\sum, \lim)
    LimitsMode limits = LimitsMode::Auto;
    bool rule = true;             // Frac: false for \binom
    int fracStyle = -1;           // Frac: -1 inherit, else a MathStyle forced on the fraction
    MathStyle style = MathStyle::Text; // StyleSwitch
    double amount = 0.0;          // Space: em; Symbol: \big scale
    std::string left;             // Delimited
    std::string right;
    int sub = -1;                 // Scripts: child indices
    int sup = -1;
    AccentKind accent = AccentKind::Hat;
    MatrixKind matrix = MatrixKind::Matrix;
    std::size_t columns = 0;
    std::size_t rows = 0;
    std::vector<MathNode> children;
};

struct ParsedMath {
    std::vector<MathNode> list; // the top-level hlist
    bool hasErrors = false;
};

std::expected<ParsedMath, std::string> parseMath(std::string_view tex, const MathLimits& limits = {});

} // namespace rivet::markdown::math
