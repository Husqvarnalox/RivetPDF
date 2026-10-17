// SPDX-License-Identifier: MPL-2.0

#include "markdown/mermaid/MermaidParser.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rivet::markdown::mermaid {

namespace {

using Error = std::unexpected<std::string>;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return s;
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsCI(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool startsWithCI(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && equalsCI(s.substr(0, prefix.size()), prefix);
}

// `s` starts with keyword `word` followed by whitespace or the end; returns
// the trimmed remainder.
std::optional<std::string_view> keyword(std::string_view s, std::string_view word, bool caseInsensitive = false) {
    if (s.size() < word.size()) return std::nullopt;
    const std::string_view head = s.substr(0, word.size());
    if (caseInsensitive ? !equalsCI(head, word) : head != word) return std::nullopt;
    if (s.size() > word.size() && !isSpace(s[word.size()])) return std::nullopt;
    return trim(s.substr(word.size()));
}

std::string lineError(std::string_view what, std::size_t line) {
    return std::string(what) + " (line " + std::to_string(line) + ")";
}

struct Line {
    std::string_view text; // trimmed
    std::size_t number = 0; // 1-based
};

// Splits into trimmed, non-empty lines without comments, directives or
// front matter.
std::vector<Line> meaningfulLines(std::string_view source) {
    std::vector<Line> out;
    std::size_t pos = 0;
    std::size_t number = 0;
    bool inFrontMatter = false;
    bool first = true;
    while (pos <= source.size()) {
        const std::size_t end = std::min(source.find('\n', pos), source.size());
        const std::string_view text = trim(source.substr(pos, end - pos));
        ++number;
        pos = end + 1;
        if (text.empty()) {
            if (end == source.size()) break;
            continue;
        }
        if (first && text == "---") {
            inFrontMatter = true;
            first = false;
            continue;
        }
        first = false;
        if (inFrontMatter) {
            if (text == "---") inFrontMatter = false;
            continue;
        }
        if (text.starts_with("%%")) continue;
        out.push_back({text, number});
        if (end == source.size()) break;
    }
    return out;
}

// Replaces <br>, <br/>, <br /> (any case) by '\n', decodes a few entities
// and splits into lines.
std::vector<std::string> labelLines(std::string_view raw) {
    std::string text;
    text.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        if (raw[i] == '<' && startsWithCI(raw.substr(i), "<br")) {
            const std::size_t close = raw.find('>', i);
            if (close != std::string_view::npos) {
                const std::string_view inner = trim(raw.substr(i + 3, close - i - 3));
                if (inner.empty() || inner == "/") {
                    text.push_back('\n');
                    i = close + 1;
                    continue;
                }
            }
        }
        if (raw[i] == '#' && raw.substr(i).starts_with("#quot;")) {
            text.push_back('"');
            i += 6;
            continue;
        }
        if (raw[i] == '&') {
            static constexpr std::pair<std::string_view, char> kEntities[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&nbsp;", ' '}};
            bool matched = false;
            for (const auto& [name, ch] : kEntities) {
                if (raw.substr(i).starts_with(name)) {
                    text.push_back(ch);
                    i += name.size();
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }
        text.push_back(raw[i] == '\t' ? ' ' : raw[i]);
        ++i;
    }
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (true) {
        const std::size_t end = std::min(text.find('\n', pos), text.size());
        lines.emplace_back(trim(std::string_view(text).substr(pos, end - pos)));
        if (end == text.size()) break;
        pos = end + 1;
    }
    return lines;
}

// Strips one level of surrounding double quotes or markdown-string backticks.
std::string_view unquote(std::string_view s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    if (s.size() >= 2 && s.front() == '`' && s.back() == '`') s = s.substr(1, s.size() - 2);
    return s;
}

// ---------------------------------------------------------------- flowchart

bool isIdByte(char c) {
    const auto u = static_cast<unsigned char>(c);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || u >= 0x80;
}

// Splits a flowchart line into statements at ';' outside labels and quotes.
std::vector<std::string_view> statements(std::string_view line) {
    std::vector<std::string_view> out;
    int depth = 0;
    bool quoted = false;
    bool piped = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') quoted = !quoted;
        if (quoted) continue;
        if (c == '|') piped = !piped;
        if (piped) continue;
        if (c == '[' || c == '(' || c == '{') ++depth;
        if ((c == ']' || c == ')' || c == '}') && depth > 0) --depth;
        if (c == ';' && depth == 0) {
            const std::string_view s = trim(line.substr(start, i - start));
            if (!s.empty()) out.push_back(s);
            start = i + 1;
        }
    }
    const std::string_view s = trim(line.substr(start));
    if (!s.empty()) out.push_back(s);
    return out;
}

struct EdgeToken {
    EdgeStroke stroke = EdgeStroke::Solid;
    bool arrow = false;
};

class FlowParser {
public:
    FlowParser(Flowchart& chart, const MermaidLimits& limits) : chart_(chart), limits_(limits) {}

    std::expected<void, std::string> statement(std::string_view s, std::size_t line) {
        if (const auto rest = keyword(s, "subgraph")) {
            if (current_ >= 0) return Error(lineError("nested subgraphs are not supported", line));
            FlowSubgraph sub;
            const std::string_view r = *rest;
            const std::size_t bracket = r.find('[');
            if (bracket != std::string_view::npos && r.back() == ']') {
                sub.id = std::string(trim(r.substr(0, bracket)));
                sub.title = std::string(unquote(r.substr(bracket + 1, r.size() - bracket - 2)));
            } else {
                sub.title = std::string(unquote(r));
                sub.id = sub.title;
            }
            if (sub.id.empty()) sub.id = "subgraph" + std::to_string(chart_.subgraphs.size() + 1);
            chart_.subgraphs.push_back(std::move(sub));
            current_ = static_cast<int>(chart_.subgraphs.size()) - 1;
            return {};
        }
        if (s == "end") {
            if (current_ < 0) return Error(lineError("'end' without a subgraph", line));
            current_ = -1;
            return {};
        }
        for (const std::string_view ignored :
             {"direction", "classDef", "class", "style", "linkStyle", "click", "accTitle", "accDescr"}) {
            if (s.starts_with(ignored) &&
                (s.size() == ignored.size() || isSpace(s[ignored.size()]) || s[ignored.size()] == ':')) {
                return {};
            }
        }
        return chain(s, line);
    }

    bool subgraphOpen() const { return current_ >= 0; }

private:
    std::expected<void, std::string> chain(std::string_view s, std::size_t line) {
        std::size_t pos = 0;
        auto left = nodeGroup(s, pos, line);
        if (!left) return Error(left.error());
        while (true) {
            skipSpaces(s, pos);
            if (pos >= s.size()) break;
            std::vector<std::string> label;
            auto edge = edgeToken(s, pos, label, line);
            if (!edge) return Error(edge.error());
            skipSpaces(s, pos);
            if (pos < s.size() && s[pos] == '|') {
                std::size_t from = pos + 1;
                skipSpaces(s, from);
                if (from < s.size() && s[from] == '"') {
                    const std::size_t quote = s.find('"', from + 1);
                    if (quote != std::string_view::npos) from = quote + 1;
                }
                const std::size_t close = s.find('|', from);
                if (close == std::string_view::npos) return Error(lineError("unterminated edge label", line));
                label = labelLines(unquote(s.substr(pos + 1, close - pos - 1)));
                pos = close + 1;
            }
            auto right = nodeGroup(s, pos, line);
            if (!right) return Error(right.error());
            for (const std::size_t from : *left) {
                for (const std::size_t to : *right) {
                    if (chart_.edges.size() >= limits_.maxEdges) return Error(std::string("too many edges"));
                    FlowEdge e;
                    e.from = from;
                    e.to = to;
                    e.stroke = edge->stroke;
                    e.arrow = edge->arrow;
                    if (!(label.size() == 1 && label.front().empty())) e.label = label;
                    chart_.edges.push_back(std::move(e));
                }
            }
            left = std::move(right);
        }
        return {};
    }

    static void skipSpaces(std::string_view s, std::size_t& pos) {
        while (pos < s.size() && isSpace(s[pos])) ++pos;
    }

    std::expected<std::vector<std::size_t>, std::string> nodeGroup(std::string_view s, std::size_t& pos,
                                                                   std::size_t line) {
        std::vector<std::size_t> group;
        while (true) {
            skipSpaces(s, pos);
            auto node = nodeRef(s, pos, line);
            if (!node) return Error(node.error());
            group.push_back(*node);
            std::size_t probe = pos;
            skipSpaces(s, probe);
            if (probe < s.size() && s[probe] == '&') {
                pos = probe + 1;
                continue;
            }
            return group;
        }
    }

    std::expected<std::size_t, std::string> nodeRef(std::string_view s, std::size_t& pos, std::size_t line) {
        const std::size_t start = pos;
        while (pos < s.size()) {
            const char c = s[pos];
            if (isIdByte(c)) {
                ++pos;
            } else if (c == '-' && pos > start && pos + 1 < s.size() && isIdByte(s[pos + 1])) {
                ++pos; // hyphenated id like "my-node"
            } else {
                break;
            }
        }
        if (pos == start) return Error(lineError("expected a node", line));
        const std::string id(s.substr(start, pos - start));

        std::optional<NodeShape> shape;
        std::string_view rawLabel;
        if (pos < s.size()) {
            struct Delim {
                std::string_view open;
                std::string_view close;
                NodeShape shape;
            };
            static constexpr Delim kDelims[] = {
                {"((", "))", NodeShape::Circle}, {"([", "])", NodeShape::Stadium}, {"[[", "]]", NodeShape::Rect},
                {"[(", ")]", NodeShape::Rect},   {"{{", "}}", NodeShape::Diamond}, {"(", ")", NodeShape::Round},
                {"[", "]", NodeShape::Rect},     {"{", "}", NodeShape::Diamond},   {">", "]", NodeShape::Rect},
            };
            for (const Delim& d : kDelims) {
                if (!s.substr(pos).starts_with(d.open)) continue;
                std::size_t from = pos + d.open.size();
                std::size_t textEnd = 0;
                std::size_t q = from;
                skipSpaces(s, q);
                if (q < s.size() && s[q] == '"') {
                    const std::size_t closeQuote = s.find('"', q + 1);
                    if (closeQuote == std::string_view::npos) return Error(lineError("unterminated quote", line));
                    rawLabel = s.substr(q + 1, closeQuote - q - 1);
                    std::size_t after = closeQuote + 1;
                    skipSpaces(s, after);
                    if (!s.substr(after).starts_with(d.close)) return Error(lineError("unterminated node label", line));
                    pos = after + d.close.size();
                } else {
                    textEnd = s.find(d.close, from);
                    if (textEnd == std::string_view::npos) return Error(lineError("unterminated node label", line));
                    rawLabel = s.substr(from, textEnd - from);
                    pos = textEnd + d.close.size();
                }
                shape = d.shape;
                break;
            }
        }

        std::size_t index = 0;
        const auto it = ids_.find(id);
        if (it == ids_.end()) {
            if (chart_.nodes.size() >= limits_.maxNodes) return Error(std::string("too many nodes"));
            FlowNode node;
            node.id = id;
            node.label = {id};
            node.subgraph = current_;
            chart_.nodes.push_back(std::move(node));
            index = chart_.nodes.size() - 1;
            ids_.emplace(id, index);
        } else {
            index = it->second;
            if (chart_.nodes[index].subgraph < 0) chart_.nodes[index].subgraph = current_;
        }
        if (shape) {
            FlowNode& node = chart_.nodes[index];
            node.shape = *shape;
            std::string_view text = rawLabel;
            // Trapezoid / parallelogram markers: [/text/], [\text\].
            if (text.size() >= 2 && (text.front() == '/' || text.front() == '\\') &&
                (text.back() == '/' || text.back() == '\\')) {
                text = text.substr(1, text.size() - 2);
            }
            node.label = labelLines(unquote(text));
        }
        return index;
    }

    std::expected<EdgeToken, std::string> edgeToken(std::string_view s, std::size_t& pos,
                                                     std::vector<std::string>& label, std::size_t line) {
        if (pos < s.size() && s[pos] == '<') ++pos; // bidirectional start: drawn as a plain arrow
        const auto readRun = [&](std::size_t& p, std::string& body, bool& head) {
            body.clear();
            while (p < s.size() && (s[p] == '-' || s[p] == '=' || s[p] == '.')) body.push_back(s[p++]);
            head = false;
            if (p < s.size() && s[p] == '>') {
                head = true;
                ++p;
            } else if (p + 1 < s.size() && (s[p] == 'x' || s[p] == 'o') && isSpace(s[p + 1]) && body.size() >= 2) {
                head = true; // cross / circle heads, drawn as arrows
                ++p;
            }
        };
        std::string body;
        bool head = false;
        std::size_t p = pos;
        readRun(p, body, head);
        if (body.empty()) return Error(lineError("expected an edge", line));

        const auto all = [&](char c) { return std::all_of(body.begin(), body.end(), [c](char b) { return b == c; }); };
        EdgeToken token;
        token.arrow = head;
        const std::size_t minLength = head ? 2 : 3;
        if (all('-') && body.size() >= minLength) {
            token.stroke = EdgeStroke::Solid;
        } else if (all('=') && body.size() >= minLength) {
            token.stroke = EdgeStroke::Thick;
        } else if (body.size() >= 3 && body.front() == '-' && body.back() == '-' &&
                   body.find('.') != std::string::npos &&
                   body.find('=') == std::string::npos) {
            token.stroke = EdgeStroke::Dotted;
        } else if (!head && (body == "--" || body == "==" || body == "-.")) {
            // Text label form: "-- text -->", "== text ==>", "-. text .->".
            const std::string_view closer = body == "--" ? "--" : body == "==" ? "==" : ".-";
            const std::size_t close = s.find(closer, p);
            if (close == std::string_view::npos) return Error(lineError("unterminated edge label", line));
            label = labelLines(unquote(s.substr(p, close - p)));
            std::size_t q = close;
            std::string closing;
            bool closingHead = false;
            readRun(q, closing, closingHead);
            const bool solidOk =
                body == "--" && closing.size() >= (closingHead ? 2u : 3u) &&
                std::all_of(closing.begin(), closing.end(), [](char c) { return c == '-'; });
            const bool thickOk =
                body == "==" && closing.size() >= (closingHead ? 2u : 3u) &&
                std::all_of(closing.begin(), closing.end(), [](char c) { return c == '='; });
            const bool dottedOk = body == "-." && closing.size() >= 2 && closing.front() == '.' &&
                                  closing.back() == '-' && closing.find('=') == std::string::npos;
            if (!solidOk && !thickOk && !dottedOk) return Error(lineError("malformed edge", line));
            token.stroke = body == "--" ? EdgeStroke::Solid : body == "==" ? EdgeStroke::Thick : EdgeStroke::Dotted;
            token.arrow = closingHead;
            p = q;
        } else {
            return Error(lineError("malformed edge", line));
        }
        pos = p;
        return token;
    }

    Flowchart& chart_;
    const MermaidLimits& limits_;
    std::unordered_map<std::string, std::size_t> ids_;
    int current_ = -1;
};

std::expected<MermaidDiagram, std::string> parseFlowchart(const std::vector<Line>& lines, std::string_view headerRest,
                                                          const MermaidLimits& limits) {
    Flowchart chart;
    // Header: "graph TD" / "flowchart LR;A-->B".
    std::string_view rest = headerRest;
    std::string_view dir = rest;
    std::string_view tail;
    const std::size_t cut = rest.find_first_of("; \t");
    if (cut != std::string_view::npos) {
        dir = rest.substr(0, cut);
        tail = trim(rest.substr(cut));
        if (!tail.empty() && tail.front() == ';') tail = trim(tail.substr(1));
    }
    if (dir.empty() || dir == "TD" || dir == "TB") {
        chart.direction = FlowDirection::TopBottom;
    } else if (dir == "BT") {
        chart.direction = FlowDirection::BottomTop;
    } else if (dir == "LR") {
        chart.direction = FlowDirection::LeftRight;
    } else if (dir == "RL") {
        chart.direction = FlowDirection::RightLeft;
    } else {
        return Error(lineError("unknown flowchart direction", lines.front().number));
    }

    FlowParser parser(chart, limits);
    const auto run = [&](std::string_view text, std::size_t number) -> std::expected<void, std::string> {
        for (const std::string_view s : statements(text)) {
            auto ok = parser.statement(s, number);
            if (!ok) return ok;
        }
        return {};
    };
    if (!tail.empty()) {
        auto ok = run(tail, lines.front().number);
        if (!ok) return Error(ok.error());
    }
    for (std::size_t i = 1; i < lines.size(); ++i) {
        auto ok = run(lines[i].text, lines[i].number);
        if (!ok) return Error(ok.error());
    }
    if (parser.subgraphOpen()) return Error(std::string("unclosed subgraph"));
    if (chart.nodes.empty()) return Error(std::string("empty diagram"));
    return chart;
}

// ----------------------------------------------------------------- sequence

class SeqParser {
public:
    SeqParser(SequenceDiagram& diagram, const MermaidLimits& limits) : d_(diagram), limits_(limits) {}

    std::expected<void, std::string> line(std::string_view s, std::size_t number) {
        for (const std::string_view ignored : {"activate", "deactivate", "title", "accTitle", "accDescr"}) {
            if (s.starts_with(ignored) &&
                (s.size() == ignored.size() || isSpace(s[ignored.size()]) || s[ignored.size()] == ':')) {
                return {};
            }
        }
        if (keyword(s, "autonumber")) {
            d_.autonumber = true;
            return {};
        }
        for (const bool actor : {false, true}) {
            if (const auto rest = keyword(s, actor ? "actor" : "participant")) {
                if (rest->empty()) return Error(lineError("expected a participant name", number));
                const std::size_t space = rest->find_first_of(" \t");
                const std::string_view id = rest->substr(0, std::min(space, rest->size()));
                std::string_view label = id;
                if (space != std::string_view::npos) {
                    const std::string_view after = trim(rest->substr(space));
                    const auto alias = keyword(after, "as");
                    if (!alias || alias->empty()) return Error(lineError("malformed participant", number));
                    label = *alias;
                }
                auto index = participant(id);
                if (!index) return Error(index.error());
                d_.participants[*index].label = std::string(label);
                d_.participants[*index].actor = actor;
                return {};
            }
        }
        if (startsWithCI(s, "note") && s.size() > 4 && isSpace(s[4])) return note(trim(s.substr(4)), number);

        static constexpr std::pair<std::string_view, SeqBlockKind> kBlocks[] = {
            {"loop", SeqBlockKind::Loop}, {"alt", SeqBlockKind::Alt},           {"opt", SeqBlockKind::Opt},
            {"par", SeqBlockKind::Par},   {"critical", SeqBlockKind::Critical}, {"break", SeqBlockKind::Break},
            {"rect", SeqBlockKind::Rect},
        };
        for (const auto& [word, kind] : kBlocks) {
            if (const auto rest = keyword(s, word)) {
                if (depth_ >= limits_.maxBlockDepth) return Error(std::string("blocks nested too deeply"));
                ++depth_;
                return push(SeqBlockStart{kind, kind == SeqBlockKind::Rect ? std::string() : std::string(*rest)});
            }
        }
        for (const std::string_view word : {"else", "and", "option"}) {
            if (const auto rest = keyword(s, word)) {
                if (depth_ == 0) return Error(lineError("'else' outside a block", number));
                return push(SeqBlockElse{std::string(*rest)});
            }
        }
        if (s == "end") {
            if (depth_ == 0) return Error(lineError("'end' without a block", number));
            --depth_;
            return push(SeqBlockEnd{});
        }
        return message(s, number);
    }

    std::size_t depth() const { return depth_; }

private:
    std::expected<std::size_t, std::string> participant(std::string_view id) {
        for (std::size_t i = 0; i < d_.participants.size(); ++i) {
            if (d_.participants[i].id == id) return i;
        }
        if (d_.participants.size() >= limits_.maxNodes) return Error(std::string("too many participants"));
        d_.participants.push_back({std::string(id), std::string(id), false});
        return d_.participants.size() - 1;
    }

    std::expected<void, std::string> push(SeqEvent event) {
        if (d_.events.size() >= limits_.maxEdges) return Error(std::string("too many messages"));
        d_.events.push_back(std::move(event));
        return {};
    }

    static std::string text(std::string_view raw) {
        const std::vector<std::string> lines = labelLines(raw);
        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) out.push_back('\n');
            out += lines[i];
        }
        return out;
    }

    std::expected<void, std::string> note(std::string_view s, std::size_t number) {
        SeqNote n;
        std::string_view rest;
        if (startsWithCI(s, "left of ")) {
            n.placement = NotePlacement::LeftOf;
            rest = s.substr(8);
        } else if (startsWithCI(s, "right of ")) {
            n.placement = NotePlacement::RightOf;
            rest = s.substr(9);
        } else if (startsWithCI(s, "over ")) {
            n.placement = NotePlacement::Over;
            rest = s.substr(5);
        } else {
            return Error(lineError("malformed note", number));
        }
        const std::size_t colon = rest.find(':');
        if (colon == std::string_view::npos) return Error(lineError("malformed note", number));
        std::string_view who = trim(rest.substr(0, colon));
        n.text = text(trim(rest.substr(colon + 1)));
        const std::size_t comma = who.find(',');
        const std::string_view a = trim(who.substr(0, std::min(comma, who.size())));
        const std::string_view b = comma == std::string_view::npos ? a : trim(who.substr(comma + 1));
        if (a.empty() || b.empty()) return Error(lineError("malformed note", number));
        if (comma != std::string_view::npos && n.placement != NotePlacement::Over) {
            return Error(lineError("malformed note", number));
        }
        auto first = participant(a);
        if (!first) return Error(first.error());
        auto last = participant(b);
        if (!last) return Error(last.error());
        n.first = std::min(*first, *last);
        n.last = std::max(*first, *last);
        return push(std::move(n));
    }

    std::expected<void, std::string> message(std::string_view s, std::size_t number) {
        struct Arrow {
            std::string_view text;
            bool dashed;
            ArrowHead head;
        };
        static constexpr Arrow kArrows[] = {
            {"-->>", true, ArrowHead::Filled}, {"--x", true, ArrowHead::Cross},    {"--)", true, ArrowHead::Open},
            {"-->", true, ArrowHead::None},    {"->>", false, ArrowHead::Filled},  {"-x", false, ArrowHead::Cross},
            {"-)", false, ArrowHead::Open},    {"->", false, ArrowHead::None},
        };
        const std::size_t colon = s.find(':');
        const std::string_view head = s.substr(0, std::min(colon, s.size()));
        for (std::size_t i = 1; i < head.size(); ++i) {
            if (head[i] != '-') continue;
            for (const Arrow& arrow : kArrows) {
                if (!head.substr(i).starts_with(arrow.text)) continue;
                const std::string_view from = trim(head.substr(0, i));
                std::string_view to = trim(head.substr(i + arrow.text.size()));
                if (!to.empty() && (to.front() == '+' || to.front() == '-')) to = trim(to.substr(1));
                if (from.empty() || to.empty()) return Error(lineError("malformed message", number));
                if (to.find_first_of(" \t") != std::string_view::npos ||
                    from.find_first_of(" \t") != std::string_view::npos) {
                    return Error(lineError("malformed message", number));
                }
                auto a = participant(from);
                if (!a) return Error(a.error());
                auto b = participant(to);
                if (!b) return Error(b.error());
                SeqMessage m;
                m.from = *a;
                m.to = *b;
                m.dashed = arrow.dashed;
                m.head = arrow.head;
                if (colon != std::string_view::npos) m.text = text(trim(s.substr(colon + 1)));
                if (d_.autonumber) m.number = ++counter_;
                return push(std::move(m));
            }
        }
        return Error(lineError("unsupported statement", number));
    }

    SequenceDiagram& d_;
    const MermaidLimits& limits_;
    std::size_t depth_ = 0;
    std::size_t counter_ = 0;
};

std::expected<MermaidDiagram, std::string> parseSequence(const std::vector<Line>& lines, const MermaidLimits& limits) {
    SequenceDiagram diagram;
    SeqParser parser(diagram, limits);
    for (std::size_t i = 1; i < lines.size(); ++i) {
        auto ok = parser.line(lines[i].text, lines[i].number);
        if (!ok) return Error(ok.error());
    }
    if (parser.depth() != 0) return Error(std::string("unclosed block"));
    if (diagram.participants.empty()) return Error(std::string("empty diagram"));
    return diagram;
}

} // namespace

std::expected<MermaidDiagram, std::string> parseMermaid(std::string_view source, const MermaidLimits& limits) {
    if (source.size() > limits.maxSourceBytes) return Error(std::string("diagram source too large"));
    if (source.find('\0') != std::string_view::npos) return Error(std::string("invalid characters"));
    const std::vector<Line> lines = meaningfulLines(source);
    if (lines.empty()) return Error(std::string("empty diagram"));
    const std::string_view header = lines.front().text;
    if (const auto rest = keyword(header, "sequenceDiagram")) {
        if (!rest->empty()) return Error(lineError("unexpected text after the diagram type", lines.front().number));
        return parseSequence(lines, limits);
    }
    for (const std::string_view word : {"flowchart", "graph"}) {
        if (header.starts_with(word) &&
            (header.size() == word.size() || isSpace(header[word.size()]) || header[word.size()] == ';')) {
            return parseFlowchart(lines, trim(header.substr(word.size())), limits);
        }
    }
    return Error(std::string("unsupported diagram type"));
}

bool isMermaidFenceInfo(std::string_view info) {
    info = trim(info);
    const std::size_t end = info.find_first_of(" \t{");
    return equalsCI(info.substr(0, std::min(end, info.size())), "mermaid");
}

} // namespace rivet::markdown::mermaid
