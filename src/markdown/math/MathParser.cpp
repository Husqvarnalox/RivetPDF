// SPDX-License-Identifier: MPL-2.0

#include "markdown/Utf8.hpp"
#include "markdown/math/MathAst.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rivet::markdown::math {

namespace {

// ---------------------------------------------------------------- tables

struct Symbol {
    std::string_view name;
    char32_t cp;
    AtomClass cls;
};

using enum AtomClass;

// Commands that stand for a single glyph.
constexpr Symbol kSymbols[] = {
    // Binary operators.
    {"pm", 0x00B1, Bin}, {"mp", 0x2213, Bin}, {"times", 0x00D7, Bin}, {"div", 0x00F7, Bin},
    {"cdot", 0x22C5, Bin}, {"ast", 0x2217, Bin}, {"star", 0x22C6, Bin}, {"circ", 0x2218, Bin},
    {"bullet", 0x2219, Bin}, {"cup", 0x222A, Bin}, {"cap", 0x2229, Bin}, {"setminus", 0x2216, Bin},
    {"wedge", 0x2227, Bin}, {"land", 0x2227, Bin}, {"vee", 0x2228, Bin}, {"lor", 0x2228, Bin},
    {"oplus", 0x2295, Bin}, {"ominus", 0x2296, Bin}, {"otimes", 0x2297, Bin}, {"oslash", 0x2298, Bin},
    {"odot", 0x2299, Bin}, {"sqcup", 0x2294, Bin}, {"sqcap", 0x2293, Bin}, {"uplus", 0x228E, Bin},
    {"amalg", 0x2A3F, Bin}, {"dagger", 0x2020, Bin}, {"wr", 0x2240, Bin},
    {"diamond", 0x22C4, Bin}, {"bigtriangleup", 0x25B3, Bin}, {"bigtriangledown", 0x25BD, Bin},
    // Relations.
    {"leq", 0x2264, Rel}, {"le", 0x2264, Rel}, {"geq", 0x2265, Rel}, {"ge", 0x2265, Rel},
    {"leqslant", 0x2A7D, Rel}, {"geqslant", 0x2A7E, Rel}, {"neq", 0x2260, Rel}, {"ne", 0x2260, Rel},
    {"approx", 0x2248, Rel}, {"equiv", 0x2261, Rel}, {"sim", 0x223C, Rel}, {"simeq", 0x2243, Rel},
    {"cong", 0x2245, Rel}, {"propto", 0x221D, Rel}, {"ll", 0x226A, Rel}, {"gg", 0x226B, Rel},
    {"in", 0x2208, Rel}, {"notin", 0x2209, Rel}, {"ni", 0x220B, Rel}, {"owns", 0x220B, Rel},
    {"subset", 0x2282, Rel}, {"supset", 0x2283, Rel}, {"subseteq", 0x2286, Rel}, {"supseteq", 0x2287, Rel},
    {"subsetneq", 0x228A, Rel}, {"supsetneq", 0x228B, Rel}, {"sqsubseteq", 0x2291, Rel},
    {"sqsupseteq", 0x2292, Rel}, {"prec", 0x227A, Rel}, {"succ", 0x227B, Rel}, {"preceq", 0x2AAF, Rel},
    {"succeq", 0x2AB0, Rel}, {"mid", 0x2223, Rel}, {"nmid", 0x2224, Rel}, {"parallel", 0x2225, Rel},
    {"perp", 0x22A5, Rel}, {"models", 0x22A8, Rel}, {"vdash", 0x22A2, Rel}, {"dashv", 0x22A3, Rel},
    {"asymp", 0x224D, Rel}, {"doteq", 0x2250, Rel}, {"bowtie", 0x22C8, Rel}, {"coloneqq", 0x2254, Rel},
    {"triangleq", 0x225C, Rel},
    // Arrows (relations).
    {"to", 0x2192, Rel}, {"rightarrow", 0x2192, Rel}, {"gets", 0x2190, Rel}, {"leftarrow", 0x2190, Rel},
    {"leftrightarrow", 0x2194, Rel}, {"Rightarrow", 0x21D2, Rel}, {"Leftarrow", 0x21D0, Rel},
    {"Leftrightarrow", 0x21D4, Rel}, {"implies", 0x27F9, Rel}, {"impliedby", 0x27F8, Rel},
    {"iff", 0x27FA, Rel}, {"mapsto", 0x21A6, Rel}, {"longmapsto", 0x27FC, Rel},
    {"longrightarrow", 0x27F6, Rel}, {"longleftarrow", 0x27F5, Rel}, {"longleftrightarrow", 0x27F7, Rel},
    {"Longrightarrow", 0x27F9, Rel}, {"Longleftarrow", 0x27F8, Rel}, {"Longleftrightarrow", 0x27FA, Rel},
    {"uparrow", 0x2191, Rel}, {"downarrow", 0x2193, Rel}, {"updownarrow", 0x2195, Rel},
    {"Uparrow", 0x21D1, Rel}, {"Downarrow", 0x21D3, Rel}, {"nearrow", 0x2197, Rel},
    {"searrow", 0x2198, Rel}, {"swarrow", 0x2199, Rel}, {"nwarrow", 0x2196, Rel},
    {"hookrightarrow", 0x21AA, Rel}, {"hookleftarrow", 0x21A9, Rel}, {"rightleftharpoons", 0x21CC, Rel},
    {"leadsto", 0x219D, Rel},
    // Ordinary symbols.
    {"infty", 0x221E, Ord}, {"partial", 0x1D715, Ord}, {"nabla", 0x2207, Ord}, {"forall", 0x2200, Ord},
    {"exists", 0x2203, Ord}, {"nexists", 0x2204, Ord}, {"emptyset", 0x2205, Ord}, {"varnothing", 0x2205, Ord},
    {"neg", 0x00AC, Ord}, {"lnot", 0x00AC, Ord}, {"ell", 0x2113, Ord}, {"hbar", 0x210F, Ord},
    {"Re", 0x211C, Ord}, {"Im", 0x2111, Ord}, {"aleph", 0x2135, Ord}, {"beth", 0x2136, Ord},
    {"angle", 0x2220, Ord}, {"triangle", 0x25B3, Ord}, {"prime", 0x2032, Ord},
    {"top", 0x22A4, Ord}, {"bot", 0x22A5, Ord}, {"square", 0x25A1, Ord}, {"Box", 0x25A1, Ord},
    {"blacksquare", 0x25A0, Ord}, {"backslash", U'\\', Ord}, {"degree", 0x2218, Ord}, {"surd", 0x221A, Ord},
    {"ldots", 0x2026, Inner}, {"dots", 0x2026, Inner}, {"cdots", 0x22EF, Inner}, {"vdots", 0x22EE, Ord},
    {"ddots", 0x22F1, Inner}, {"dotsb", 0x22EF, Inner}, {"dotsc", 0x2026, Inner},
    // Delimiters as plain symbols.
    {"langle", 0x27E8, Open}, {"rangle", 0x27E9, Close}, {"lfloor", 0x230A, Open}, {"rfloor", 0x230B, Close},
    {"lceil", 0x2308, Open}, {"rceil", 0x2309, Close}, {"lbrace", U'{', Open}, {"rbrace", U'}', Close},
    {"lvert", U'|', Open}, {"rvert", U'|', Close}, {"lVert", 0x2016, Open}, {"rVert", 0x2016, Close},
    {"vert", U'|', Ord}, {"Vert", 0x2016, Ord}, {"lbrack", U'[', Open}, {"rbrack", U']', Close},
    // Punctuation.
    {"colon", U':', Punct},
};

struct Greek {
    std::string_view name;
    char32_t cp; // upright
};

constexpr Greek kGreek[] = {
    {"alpha", 0x3B1}, {"beta", 0x3B2},   {"gamma", 0x3B3},    {"delta", 0x3B4},    {"epsilon", 0x3F5},
    {"varepsilon", 0x3B5}, {"zeta", 0x3B6}, {"eta", 0x3B7},   {"theta", 0x3B8},    {"vartheta", 0x3D1},
    {"iota", 0x3B9}, {"kappa", 0x3BA},   {"varkappa", 0x3F0}, {"lambda", 0x3BB},   {"mu", 0x3BC},
    {"nu", 0x3BD},   {"xi", 0x3BE},      {"omicron", 0x3BF},  {"pi", 0x3C0},       {"varpi", 0x3D6},
    {"rho", 0x3C1},  {"varrho", 0x3F1},  {"sigma", 0x3C3},    {"varsigma", 0x3C2}, {"tau", 0x3C4},
    {"upsilon", 0x3C5}, {"phi", 0x3D5},  {"varphi", 0x3C6},   {"chi", 0x3C7},      {"psi", 0x3C8},
    {"omega", 0x3C9}, {"Gamma", 0x393},  {"Delta", 0x394},    {"Theta", 0x398},    {"Lambda", 0x39B},
    {"Xi", 0x39E},   {"Pi", 0x3A0},      {"Sigma", 0x3A3},    {"Upsilon", 0x3A5},  {"Phi", 0x3A6},
    {"Psi", 0x3A8},  {"Omega", 0x3A9},
};

struct BigOperator {
    std::string_view name;
    char32_t cp;
    bool limits; // limits in display style
};

constexpr BigOperator kBigOps[] = {
    {"sum", 0x2211, true},       {"prod", 0x220F, true},      {"coprod", 0x2210, true},
    {"int", 0x222B, false},      {"iint", 0x222C, false},     {"iiint", 0x222D, false},
    {"oint", 0x222E, false},     {"bigcup", 0x22C3, true},    {"bigcap", 0x22C2, true},
    {"bigoplus", 0x2A01, true},  {"bigotimes", 0x2A02, true}, {"bigodot", 0x2A00, true},
    {"bigvee", 0x22C1, true},    {"bigwedge", 0x22C0, true},  {"bigsqcup", 0x2A06, true},
    {"biguplus", 0x2A04, true},
};

struct NamedOperator {
    std::string_view name;
    std::string_view text;
    bool limits;
};

constexpr NamedOperator kNamedOps[] = {
    {"arccos", "arccos", false}, {"arcsin", "arcsin", false}, {"arctan", "arctan", false},
    {"arg", "arg", false},       {"cos", "cos", false},       {"cosh", "cosh", false},
    {"cot", "cot", false},       {"coth", "coth", false},     {"csc", "csc", false},
    {"deg", "deg", false},       {"dim", "dim", false},       {"exp", "exp", false},
    {"hom", "hom", false},       {"ker", "ker", false},       {"lg", "lg", false},
    {"ln", "ln", false},         {"log", "log", false},       {"sec", "sec", false},
    {"sin", "sin", false},       {"sinh", "sinh", false},     {"tan", "tan", false},
    {"tanh", "tanh", false},     {"det", "det", true},        {"gcd", "gcd", true},
    {"inf", "inf", true},        {"lim", "lim", true},        {"liminf", "lim inf", true},
    {"limsup", "lim sup", true}, {"max", "max", true},        {"min", "min", true},
    {"Pr", "Pr", true},          {"sup", "sup", true},        {"argmax", "arg max", true},
    {"argmin", "arg min", true},
};

struct Negation {
    std::string_view name; // command name, or "=" for the character
    char32_t cp;
};

constexpr Negation kNegations[] = {
    {"=", 0x2260},      {"in", 0x2209},     {"ni", 0x220C},       {"subset", 0x2284},  {"supset", 0x2285},
    {"subseteq", 0x2288}, {"supseteq", 0x2289}, {"equiv", 0x2262}, {"sim", 0x2241},     {"approx", 0x2249},
    {"leq", 0x2270},    {"le", 0x2270},     {"geq", 0x2271},      {"ge", 0x2271},      {"cong", 0x2247},
    {"exists", 0x2204}, {"mid", 0x2224},    {"parallel", 0x2226}, {"<", 0x226E},       {">", 0x226F},
    {"simeq", 0x2244},  {"prec", 0x2280},   {"succ", 0x2281},
};

enum class Variant : std::uint8_t { Default, Roman, Bold, BoldItalic, Blackboard, Calligraphic };

char32_t italicLatin(char32_t c) {
    if (c == U'h') return 0x210E;
    if (c >= U'a' && c <= U'z') return 0x1D44E + (c - U'a');
    if (c >= U'A' && c <= U'Z') return 0x1D434 + (c - U'A');
    return c;
}

char32_t italicGreek(char32_t c) {
    if (c >= 0x3B1 && c <= 0x3C9) return 0x1D6FC + (c - 0x3B1);
    switch (c) {
    case 0x3F5: return 0x1D716;
    case 0x3D1: return 0x1D717;
    case 0x3F0: return 0x1D718;
    case 0x3D5: return 0x1D719;
    case 0x3F1: return 0x1D71A;
    case 0x3D6: return 0x1D71B;
    default: return c;
    }
}

char32_t mapVariant(char32_t c, Variant v) {
    const bool lower = c >= U'a' && c <= U'z';
    const bool upper = c >= U'A' && c <= U'Z';
    const bool digit = c >= U'0' && c <= U'9';
    const bool greekLower = (c >= 0x3B1 && c <= 0x3C9) || c == 0x3F5 || c == 0x3D1 || c == 0x3F0 || c == 0x3D5 ||
                            c == 0x3F1 || c == 0x3D6;
    const bool greekUpper = c >= 0x391 && c <= 0x3A9 && c != 0x3A2;
    switch (v) {
    case Variant::Default:
        if (lower || upper) return italicLatin(c);
        if (greekLower) return italicGreek(c);
        return c;
    case Variant::Roman: return c;
    case Variant::Bold:
        if (upper) return 0x1D400 + (c - U'A');
        if (lower) return 0x1D41A + (c - U'a');
        if (digit) return 0x1D7CE + (c - U'0');
        if (greekUpper) return 0x1D6A8 + (c - 0x391);
        if (c >= 0x3B1 && c <= 0x3C9) return 0x1D6C2 + (c - 0x3B1);
        return c;
    case Variant::BoldItalic:
        if (upper) return 0x1D468 + (c - U'A');
        if (lower) return 0x1D482 + (c - U'a');
        if (digit) return 0x1D7CE + (c - U'0');
        if (greekUpper) return 0x1D71C + (c - 0x391);
        if (c >= 0x3B1 && c <= 0x3C9) return 0x1D736 + (c - 0x3B1);
        return c;
    case Variant::Blackboard:
        if (upper) {
            switch (c) {
            case U'C': return 0x2102;
            case U'H': return 0x210D;
            case U'N': return 0x2115;
            case U'P': return 0x2119;
            case U'Q': return 0x211A;
            case U'R': return 0x211D;
            case U'Z': return 0x2124;
            default: return 0x1D538 + (c - U'A');
            }
        }
        if (lower) return 0x1D552 + (c - U'a');
        if (digit) return 0x1D7D8 + (c - U'0');
        return c;
    case Variant::Calligraphic:
        if (upper) {
            switch (c) {
            case U'B': return 0x212C;
            case U'E': return 0x2130;
            case U'F': return 0x2131;
            case U'H': return 0x210B;
            case U'I': return 0x2110;
            case U'L': return 0x2112;
            case U'M': return 0x2133;
            case U'R': return 0x211B;
            default: return 0x1D49C + (c - U'A');
            }
        }
        if (lower) {
            switch (c) {
            case U'e': return 0x212F;
            case U'g': return 0x210A;
            case U'o': return 0x2134;
            default: return 0x1D4B6 + (c - U'a');
            }
        }
        return c;
    }
    return c;
}

std::string utf8Of(char32_t cp) {
    std::string s;
    utf8::append(s, cp);
    return s;
}

// The class of a code point typed directly (or produced by a table).
AtomClass classOf(char32_t cp) {
    switch (cp) {
    case U'+': case U'*': return Bin;
    case 0x2212: return Bin;
    case U'=': case U'<': case U'>': case U':': return Rel;
    case U'(': case U'[': return Open;
    case U')': case U']': case U'!': case U'?': return Close;
    case U',': case U';': return Punct;
    default: break;
    }
    for (const Symbol& s : kSymbols) {
        if (s.cp == cp) return s.cls;
    }
    return Ord;
}

// ------------------------------------------------------------- tokenizer

struct Token {
    enum class Type : std::uint8_t { End, Char, Command, Open, Close, Sup, Sub, Amp, Prime };
    Type type = Type::End;
    char32_t cp = 0;
    std::string_view name; // Command
};

bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

class Parser {
public:
    Parser(std::string_view src, const MathLimits& limits) : s_(src), limits_(limits) {}

    std::expected<ParsedMath, std::string> run() {
        ParsedMath out;
        out.list = parseList(Stop::Top);
        if (failure_) return std::unexpected(*failure_);
        out.hasErrors = hasErrors_;
        return out;
    }

private:
    enum class Stop : std::uint8_t { Top, Group, Bracket, Right, Cell };

    // ---- tokens

    void skipSpace() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else if (c == '%') {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    Token lex(std::size_t& p) const {
        Token t;
        if (p >= s_.size()) return t;
        const char c = s_[p];
        switch (c) {
        case '{': ++p; t.type = Token::Type::Open; return t;
        case '}': ++p; t.type = Token::Type::Close; return t;
        case '^': ++p; t.type = Token::Type::Sup; return t;
        case '_': ++p; t.type = Token::Type::Sub; return t;
        case '&': ++p; t.type = Token::Type::Amp; return t;
        case '\'': ++p; t.type = Token::Type::Prime; return t;
        case '\\': {
            t.type = Token::Type::Command;
            const std::size_t start = p + 1;
            std::size_t q = start;
            if (q < s_.size() && isLetter(s_[q])) {
                while (q < s_.size() && isLetter(s_[q])) ++q;
            } else if (q < s_.size()) {
                const auto d = utf8::decode(s_, q);
                q += d.length;
            }
            t.name = s_.substr(start, q - start);
            p = q;
            return t;
        }
        default: {
            const auto d = utf8::decode(s_, p);
            p += d.length;
            t.type = Token::Type::Char;
            t.cp = d.codePoint;
            return t;
        }
        }
    }

    Token peek() {
        skipSpace();
        std::size_t p = pos_;
        return lex(p);
    }

    Token get() {
        skipSpace();
        return lex(pos_);
    }

    bool peekChar(char32_t cp) {
        const Token t = peek();
        return t.type == Token::Type::Char && t.cp == cp;
    }

    bool peekCommand(std::string_view name) {
        const Token t = peek();
        return t.type == Token::Type::Command && t.name == name;
    }

    // ---- nodes

    void fail(std::string reason) {
        if (!failure_) failure_ = std::move(reason);
    }

    MathNode node(MathNode::Kind kind) {
        if (++nodes_ > limits_.maxNodes) fail("formula too large");
        MathNode n;
        n.kind = kind;
        return n;
    }

    MathNode symbol(char32_t cp, AtomClass cls) {
        MathNode n = node(MathNode::Kind::Symbol);
        n.text = utf8Of(cp);
        n.cls = cls;
        return n;
    }

    MathNode error(std::string literal) {
        hasErrors_ = true;
        MathNode n = node(MathNode::Kind::Error);
        n.text = std::move(literal);
        n.font = MathFont::Text;
        return n;
    }

    MathNode space(double em) {
        MathNode n = node(MathNode::Kind::Space);
        n.amount = em;
        return n;
    }

    MathNode group(std::vector<MathNode> list) {
        MathNode n = node(MathNode::Kind::Group);
        n.children = std::move(list);
        return n;
    }

    // ---- lists

    std::vector<MathNode> parseList(Stop stop) {
        std::vector<MathNode> list;
        if (++depth_ > limits_.maxDepth) {
            fail("formula nested too deeply");
            --depth_;
            return list;
        }
        while (!failure_) {
            const Token t = peek();
            if (t.type == Token::Type::End) break;
            if (t.type == Token::Type::Close) {
                if (stop != Stop::Top) break;
                get();
                list.push_back(error("}"));
                continue;
            }
            if (stop == Stop::Bracket && t.type == Token::Type::Char && t.cp == U']') break;
            if (t.type == Token::Type::Command && t.name == "right") {
                if (stop == Stop::Right) break;
                get();
                readDelimiter();
                list.push_back(error("\\right"));
                continue;
            }
            if (t.type == Token::Type::Command && t.name == "end") {
                if (stop == Stop::Cell) break;
                get();
                const std::string name = readRawText();
                list.push_back(error("\\end{" + name + "}"));
                continue;
            }
            if (t.type == Token::Type::Amp || (t.type == Token::Type::Command && t.name == "\\")) {
                if (stop == Stop::Cell) break;
                get();
                if (t.type == Token::Type::Amp) list.push_back(error("&"));
                continue; // a line break outside an environment is ignored
            }
            parseAtom(list);
        }
        --depth_;
        return list;
    }

    void parseAtom(std::vector<MathNode>& list) {
        MathNode base;
        const bool hasBase = parsePrimary(base);
        std::optional<MathNode> sub;
        std::optional<MathNode> sup;
        bool supIsPrimes = false;
        while (!failure_) {
            const Token t = peek();
            if (t.type == Token::Type::Prime) {
                std::size_t count = 0;
                while (peek().type == Token::Type::Prime) {
                    get();
                    ++count;
                }
                std::string primes;
                if (count == 2) {
                    primes = utf8Of(0x2033);
                } else if (count == 3) {
                    primes = utf8Of(0x2034);
                } else {
                    for (std::size_t i = 0; i < count; ++i) primes += utf8Of(0x2032);
                }
                MathNode p = node(MathNode::Kind::Symbol);
                p.text = std::move(primes);
                if (sup) {
                    list.push_back(error("'"));
                } else {
                    sup = std::move(p);
                    supIsPrimes = true;
                }
            } else if (t.type == Token::Type::Sup) {
                get();
                MathNode arg = parseArg();
                if (sup && supIsPrimes) {
                    std::vector<MathNode> both;
                    both.push_back(std::move(*sup));
                    both.push_back(std::move(arg));
                    sup = group(std::move(both));
                    supIsPrimes = false;
                } else if (sup) {
                    hasErrors_ = true; // double superscript: the later one wins
                    sup = std::move(arg);
                } else {
                    sup = std::move(arg);
                }
            } else if (t.type == Token::Type::Sub) {
                get();
                MathNode arg = parseArg();
                if (sub) hasErrors_ = true;
                sub = std::move(arg);
            } else {
                break;
            }
        }
        if (!sub && !sup) {
            if (hasBase) list.push_back(std::move(base));
            return;
        }
        MathNode scripts = node(MathNode::Kind::Scripts);
        scripts.children.push_back(hasBase ? std::move(base) : group({}));
        if (sub) {
            scripts.sub = static_cast<int>(scripts.children.size());
            scripts.children.push_back(std::move(*sub));
        }
        if (sup) {
            scripts.sup = static_cast<int>(scripts.children.size());
            scripts.children.push_back(std::move(*sup));
        }
        list.push_back(std::move(scripts));
    }

    // A required argument: a {group} or a single token.
    MathNode parseArg() {
        const Token t = peek();
        switch (t.type) {
        case Token::Type::Open: {
            get();
            MathNode g = group(parseList(Stop::Group));
            expectClose();
            return g;
        }
        case Token::Type::Char:
        case Token::Type::Command:
        case Token::Type::Prime: {
            MathNode n;
            if (parsePrimary(n)) return n;
            return group({});
        }
        case Token::Type::End:
        case Token::Type::Close:
        case Token::Type::Sup:
        case Token::Type::Sub:
        case Token::Type::Amp:
            break;
        }
        hasErrors_ = true; // missing argument
        return group({});
    }

    void expectClose() {
        if (peek().type == Token::Type::Close) {
            get();
        } else {
            hasErrors_ = true; // unbalanced: closed at the end of the input
        }
    }

    // Raw text of a {group} (or of one token) for \text and names; TeX
    // escapes are resolved, inner braces dropped.
    std::string readRawText() {
        skipSpace();
        std::string out;
        if (pos_ >= s_.size()) {
            hasErrors_ = true;
            return out;
        }
        if (s_[pos_] != '{') {
            const Token t = get();
            if (t.type == Token::Type::Char) out = utf8Of(t.cp);
            if (t.type == Token::Type::Command) out = "\\" + std::string(t.name);
            return out;
        }
        ++pos_;
        int depth = 1;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == '\\' && pos_ + 1 < s_.size()) {
                const char n = s_[pos_ + 1];
                if (n == '{' || n == '}' || n == '$' || n == '%' || n == '&' || n == '_' || n == '#' || n == ' ' ||
                    n == '\\') {
                    out.push_back(n == '\\' ? ' ' : n);
                    pos_ += 2;
                    continue;
                }
            }
            if (c == '{') {
                ++depth;
                ++pos_;
                continue;
            }
            if (c == '}') {
                ++pos_;
                if (--depth == 0) return out;
                continue;
            }
            out.push_back(c == '\n' || c == '\t' || c == '\r' ? ' ' : c);
            ++pos_;
        }
        hasErrors_ = true; // unterminated
        return out;
    }

    // A delimiter after \left, \right, \big...: its glyph, "" for '.'.
    std::string readDelimiter() {
        const Token t = get();
        if (t.type == Token::Type::Char) {
            switch (t.cp) {
            case U'(': case U')': case U'[': case U']': case U'|': case U'/': return utf8Of(t.cp);
            case U'<': return utf8Of(0x27E8);
            case U'>': return utf8Of(0x27E9);
            case U'.': return {};
            default: break;
            }
        } else if (t.type == Token::Type::Command) {
            if (t.name == "{" || t.name == "lbrace") return "{";
            if (t.name == "}" || t.name == "rbrace") return "}";
            if (t.name == "|" || t.name == "Vert" || t.name == "lVert" || t.name == "rVert") return utf8Of(0x2016);
            if (t.name == "vert" || t.name == "lvert" || t.name == "rvert") return "|";
            if (t.name == "backslash") return "\\";
            for (const std::string_view name : {"langle", "rangle", "lfloor", "rfloor", "lceil", "rceil", "uparrow",
                                                "downarrow", "updownarrow", "Uparrow", "Downarrow", "lbrack",
                                                "rbrack"}) {
                if (t.name == name) {
                    for (const Symbol& s : kSymbols) {
                        if (s.name == name) return utf8Of(s.cp);
                    }
                }
            }
        }
        hasErrors_ = true;
        return {};
    }

    // ---- primaries

    bool parsePrimary(MathNode& out) {
        const Token t = get();
        switch (t.type) {
        case Token::Type::End: return false;
        case Token::Type::Open:
            out = group(parseList(Stop::Group));
            expectClose();
            return true;
        case Token::Type::Close: out = error("}"); return true;
        case Token::Type::Sup: out = error("^"); return true;
        case Token::Type::Sub: out = error("_"); return true;
        case Token::Type::Amp: out = error("&"); return true;
        case Token::Type::Prime: out = symbol(0x2032, Ord); return true;
        case Token::Type::Char: return character(t.cp, out);
        case Token::Type::Command: return command(t.name, out);
        }
        return false;
    }

    bool character(char32_t cp, MathNode& out) {
        if (cp == U'~') {
            out = space(0.333);
            return true;
        }
        if (cp == U'-') {
            out = symbol(0x2212, Bin);
            return true;
        }
        if (cp == U'*') {
            out = symbol(0x2217, Bin);
            return true;
        }
        if (cp == U'`') {
            out = symbol(0x2018, Ord);
            return true;
        }
        if (cp == U'<' || cp == U'>' || cp == U'=') {
            out = symbol(cp, Rel);
            return true;
        }
        const AtomClass cls = classOf(cp);
        out = symbol(mapVariant(cp, variant_), cls);
        return true;
    }

    MathNode withVariant(Variant v) {
        const Variant saved = variant_;
        variant_ = v;
        MathNode arg = parseArg();
        variant_ = saved;
        if (arg.kind != MathNode::Kind::Group) {
            std::vector<MathNode> one;
            one.push_back(std::move(arg));
            return group(std::move(one));
        }
        return arg;
    }

    bool command(std::string_view name, MathNode& out) {
        // Escaped characters and spacing.
        if (name.size() == 1 || (!name.empty() && !isLetter(name.front()))) {
            if (name == "{") { out = symbol(U'{', Open); return true; }
            if (name == "}") { out = symbol(U'}', Close); return true; }
            if (name == "|") { out = symbol(0x2016, Ord); return true; }
            if (name == "%" || name == "$" || name == "&" || name == "#" || name == "_") {
                out = symbol(static_cast<char32_t>(name.front()), Ord);
                return true;
            }
            if (name == ",") { out = space(3.0 / 18.0); return true; }
            if (name == ":" || name == ">") { out = space(4.0 / 18.0); return true; }
            if (name == ";") { out = space(5.0 / 18.0); return true; }
            if (name == "!") { out = space(-3.0 / 18.0); return true; }
            if (name == " ") { out = space(0.333); return true; }
            if (name == "\\") return false;
        }
        for (const Greek& g : kGreek) {
            if (g.name == name) {
                out = symbol(mapVariant(g.cp, variant_ == Variant::Roman ? Variant::Roman : variant_), Ord);
                return true;
            }
        }
        for (const Symbol& s : kSymbols) {
            if (s.name == name) {
                out = symbol(s.cp, s.cls);
                return true;
            }
        }
        for (const BigOperator& op : kBigOps) {
            if (op.name == name) {
                out = node(MathNode::Kind::BigOp);
                out.cls = Op;
                out.text = utf8Of(op.cp);
                out.limitsInDisplay = op.limits;
                readLimits(out);
                return true;
            }
        }
        for (const NamedOperator& op : kNamedOps) {
            if (op.name == name) {
                out = node(MathNode::Kind::BigOp);
                out.cls = Op;
                out.named = true;
                out.text = std::string(op.text);
                out.limitsInDisplay = op.limits;
                readLimits(out);
                return true;
            }
        }
        if (name == "operatorname") {
            bool star = false;
            if (peekChar(U'*')) {
                get();
                star = true;
            }
            out = node(MathNode::Kind::BigOp);
            out.cls = Op;
            out.named = true;
            out.text = readRawText();
            out.limitsInDisplay = star;
            readLimits(out);
            return true;
        }
        if (name == "frac" || name == "dfrac" || name == "tfrac" || name == "cfrac") {
            out = node(MathNode::Kind::Frac);
            out.cls = Inner;
            out.fracStyle = name == "dfrac" || name == "cfrac" ? static_cast<int>(MathStyle::Display)
                            : name == "tfrac"                  ? static_cast<int>(MathStyle::Text)
                                                               : -1;
            out.children.push_back(parseArg());
            out.children.push_back(parseArg());
            return true;
        }
        if (name == "binom" || name == "dbinom" || name == "tbinom") {
            MathNode frac = node(MathNode::Kind::Frac);
            frac.rule = false;
            frac.fracStyle = name == "dbinom"   ? static_cast<int>(MathStyle::Display)
                             : name == "tbinom" ? static_cast<int>(MathStyle::Text)
                                                : -1;
            frac.children.push_back(parseArg());
            frac.children.push_back(parseArg());
            out = node(MathNode::Kind::Delimited);
            out.cls = Inner;
            out.left = "(";
            out.right = ")";
            std::vector<MathNode> body;
            body.push_back(std::move(frac));
            out.children.push_back(group(std::move(body)));
            return true;
        }
        if (name == "sqrt") {
            out = node(MathNode::Kind::Sqrt);
            std::optional<MathNode> index;
            if (peekChar(U'[')) {
                get();
                index = group(parseList(Stop::Bracket));
                if (peekChar(U']')) {
                    get();
                } else {
                    hasErrors_ = true;
                }
            }
            out.children.push_back(parseArg());
            if (index) out.children.push_back(std::move(*index));
            return true;
        }
        if (name == "left") {
            out = node(MathNode::Kind::Delimited);
            out.cls = Inner;
            out.left = readDelimiter();
            out.children.push_back(group(parseList(Stop::Right)));
            if (peekCommand("right")) {
                get();
                out.right = readDelimiter();
            } else {
                hasErrors_ = true; // missing \right: a null delimiter
            }
            return true;
        }
        if (name == "middle") {
            out = node(MathNode::Kind::Symbol);
            out.text = readDelimiter();
            out.cls = Ord;
            if (out.text.empty()) out = space(0.12);
            return true;
        }
        if (name.starts_with("big") || name.starts_with("Big")) {
            const std::string_view base = name.substr(0, name.size() >= 4 && name[3] == 'g' ? 4 : 3);
            const std::string_view suffix = name.substr(base.size());
            double scale = 0.0;
            if (base == "big") scale = 1.2;
            if (base == "Big") scale = 1.8;
            if (base == "bigg") scale = 2.4;
            if (base == "Bigg") scale = 3.0;
            AtomClass cls = Ord;
            bool okSuffix = true;
            if (suffix == "l") {
                cls = Open;
            } else if (suffix == "r") {
                cls = Close;
            } else if (suffix == "m") {
                cls = Rel;
            } else if (!suffix.empty()) {
                okSuffix = false;
            }
            if (scale > 0.0 && okSuffix) {
                const std::string glyph = readDelimiter();
                if (glyph.empty()) {
                    out = space(0.12);
                    return true;
                }
                out = node(MathNode::Kind::Symbol);
                out.text = glyph;
                out.cls = cls;
                out.amount = scale;
                return true;
            }
        }
        if (name == "text" || name == "textrm" || name == "textup" || name == "textnormal" || name == "mbox" ||
            name == "textit" || name == "textsf" || name == "texttt" || name == "hbox") {
            out = node(MathNode::Kind::Text);
            out.font = MathFont::Text;
            out.text = readRawText();
            return true;
        }
        if (name == "textbf") {
            out = node(MathNode::Kind::Text);
            out.font = MathFont::TextBold;
            out.text = readRawText();
            return true;
        }
        if (name == "mathbf") { out = withVariant(Variant::Bold); return true; }
        if (name == "boldsymbol" || name == "bm") { out = withVariant(Variant::BoldItalic); return true; }
        if (name == "mathit") { out = withVariant(Variant::Default); return true; }
        if (name == "mathrm" || name == "mathsf" || name == "mathtt" || name == "mathup" || name == "mathfrak") {
            out = withVariant(Variant::Roman);
            return true;
        }
        if (name == "mathbb") { out = withVariant(Variant::Blackboard); return true; }
        if (name == "mathcal" || name == "mathscr") { out = withVariant(Variant::Calligraphic); return true; }

        struct AccentName {
            std::string_view name;
            AccentKind kind;
        };
        static constexpr AccentName kAccents[] = {
            {"hat", AccentKind::Hat},         {"widehat", AccentKind::Hat},      {"tilde", AccentKind::Tilde},
            {"widetilde", AccentKind::Tilde}, {"bar", AccentKind::Bar},          {"overline", AccentKind::Overline},
            {"underline", AccentKind::Underline}, {"vec", AccentKind::Vec},     {"overrightarrow", AccentKind::Vec},
            {"dot", AccentKind::Dot},         {"ddot", AccentKind::DDot},        {"check", AccentKind::Check},
        };
        for (const AccentName& a : kAccents) {
            if (a.name == name) {
                out = node(MathNode::Kind::Accent);
                out.accent = a.kind;
                out.children.push_back(parseArg());
                return true;
            }
        }
        if (name == "begin") return environment(out);
        if (name == "not") {
            const Token t = peek();
            for (const Negation& neg : kNegations) {
                const bool match = (t.type == Token::Type::Command && t.name == neg.name) ||
                                   (t.type == Token::Type::Char && neg.name.size() == 1 &&
                                    t.cp == static_cast<char32_t>(neg.name.front()));
                if (match) {
                    get();
                    out = symbol(neg.cp, Rel);
                    return true;
                }
            }
            out = error("\\not");
            return true;
        }
        if (name == "quad") { out = space(1.0); return true; }
        if (name == "qquad") { out = space(2.0); return true; }
        if (name == "enspace") { out = space(0.5); return true; }
        if (name == "thinspace") { out = space(3.0 / 18.0); return true; }
        if (name == "medspace") { out = space(4.0 / 18.0); return true; }
        if (name == "thickspace") { out = space(5.0 / 18.0); return true; }
        if (name == "negthinspace") { out = space(-3.0 / 18.0); return true; }
        struct StyleName {
            std::string_view name;
            MathStyle style;
        };
        static constexpr StyleName kStyles[] = {{"displaystyle", MathStyle::Display},
                                                {"textstyle", MathStyle::Text},
                                                {"scriptstyle", MathStyle::Script},
                                                {"scriptscriptstyle", MathStyle::ScriptScript}};
        for (const StyleName& st : kStyles) {
            if (st.name == name) {
                out = node(MathNode::Kind::StyleSwitch);
                out.style = st.style;
                return true;
            }
        }
        if (name == "phantom") {
            out = node(MathNode::Kind::Phantom);
            out.children.push_back(parseArg());
            return true;
        }
        if (name == "color") {
            readRawText();
            return false;
        }
        if (name == "textcolor") {
            readRawText();
            out = parseArg();
            return true;
        }
        if (name == "limits" || name == "nolimits" || name == "hline" || name == "nonumber" || name == "notag" ||
            name == "displaylimits") {
            return false;
        }
        if (name == "tag") {
            readRawText();
            return false;
        }
        out = error("\\" + std::string(name));
        return true;
    }

    void readLimits(MathNode& op) {
        while (true) {
            if (peekCommand("limits")) {
                get();
                op.limits = LimitsMode::Limits;
            } else if (peekCommand("nolimits")) {
                get();
                op.limits = LimitsMode::NoLimits;
            } else if (peekCommand("displaylimits")) {
                get();
                op.limits = LimitsMode::Auto;
            } else {
                return;
            }
        }
    }

    bool environment(MathNode& out) {
        const std::string name = readRawText();
        struct Env {
            std::string_view name;
            MatrixKind kind;
        };
        static constexpr Env kEnvs[] = {
            {"matrix", MatrixKind::Matrix},    {"pmatrix", MatrixKind::Paren},      {"bmatrix", MatrixKind::Bracket},
            {"Bmatrix", MatrixKind::Brace},    {"vmatrix", MatrixKind::Vert},       {"Vmatrix", MatrixKind::DoubleVert},
            {"smallmatrix", MatrixKind::Small}, {"cases", MatrixKind::Cases},       {"aligned", MatrixKind::Aligned},
            {"align", MatrixKind::Aligned},    {"align*", MatrixKind::Aligned},     {"split", MatrixKind::Aligned},
            {"alignedat", MatrixKind::Aligned}, {"gathered", MatrixKind::Gathered}, {"gather", MatrixKind::Gathered},
            {"gather*", MatrixKind::Gathered}, {"array", MatrixKind::Matrix},       {"equation", MatrixKind::Gathered},
            {"equation*", MatrixKind::Gathered},
        };
        const Env* env = nullptr;
        for (const Env& e : kEnvs) {
            if (e.name == name) env = &e;
        }
        if (env == nullptr) {
            out = error("\\begin{" + name + "}");
            return true;
        }
        if (name == "array" || name == "alignedat") readRawText(); // column spec / column count: ignored

        out = node(MathNode::Kind::Matrix);
        out.matrix = env->kind;
        std::vector<std::vector<MathNode>> rows(1);
        std::size_t cells = 0;
        while (!failure_) {
            if (++cells > limits_.maxMatrixCells) {
                fail("matrix too large");
                break;
            }
            rows.back().push_back(group(parseList(Stop::Cell)));
            const Token t = peek();
            if (t.type == Token::Type::Amp) {
                get();
                continue;
            }
            if (t.type == Token::Type::Command && t.name == "\\") {
                get();
                if (peekChar(U'[')) { // optional vertical space: ignored
                    while (true) {
                        const Token skip = get();
                        if (skip.type == Token::Type::End || (skip.type == Token::Type::Char && skip.cp == U']')) break;
                    }
                }
                rows.emplace_back();
                continue;
            }
            if (t.type == Token::Type::Command && t.name == "end") {
                get();
                if (readRawText() != name) hasErrors_ = true;
            } else {
                hasErrors_ = true; // unterminated environment
            }
            break;
        }
        // A trailing "\\" leaves an empty last row.
        const auto emptyRow = [](const std::vector<MathNode>& row) {
            return row.size() == 1 && row.front().children.empty();
        };
        if (rows.size() > 1 && emptyRow(rows.back())) rows.pop_back();
        std::size_t columns = 0;
        for (const auto& row : rows) columns = std::max(columns, row.size());
        out.rows = rows.size();
        out.columns = columns;
        for (auto& row : rows) {
            while (row.size() < columns) row.push_back(group({}));
            for (MathNode& cell : row) out.children.push_back(std::move(cell));
        }
        return true;
    }

    std::string_view s_;
    const MathLimits& limits_;
    std::size_t pos_ = 0;
    std::size_t depth_ = 0;
    std::size_t nodes_ = 0;
    bool hasErrors_ = false;
    Variant variant_ = Variant::Default;
    std::optional<std::string> failure_;
};

} // namespace

std::expected<ParsedMath, std::string> parseMath(std::string_view tex, const MathLimits& limits) {
    if (tex.size() > limits.maxSourceBytes) return std::unexpected(std::string("formula too large"));
    Parser parser(tex, limits);
    return parser.run();
}

} // namespace rivet::markdown::math
