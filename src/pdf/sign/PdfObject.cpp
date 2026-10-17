// SPDX-License-Identifier: MPL-2.0
#include "pdf/sign/PdfObject.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace rivet::pdf::sign {

namespace {

SignError malformed(const char* what) { return SignError{SignErrorCode::MalformedPdf, what, std::nullopt}; }
std::unexpected<SignError> bad(const char* what) { return std::unexpected<SignError>(malformed(what)); }

bool isRegular(std::uint8_t c) { return !isPdfWhitespace(c) && !isPdfDelimiter(c); }
bool isDigit(std::uint8_t c) { return c >= '0' && c <= '9'; }

int hexValue(std::uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

// ---- PdfObject factories ---------------------------------------------------------

PdfObject PdfObject::makeBool(bool v) {
    PdfObject o;
    o.kind = Kind::Bool;
    o.boolean = v;
    return o;
}
PdfObject PdfObject::makeInt(std::int64_t v) {
    PdfObject o;
    o.kind = Kind::Int;
    o.integer = v;
    return o;
}
PdfObject PdfObject::makeReal(std::string lexeme) {
    PdfObject o;
    o.kind = Kind::Real;
    o.text = std::move(lexeme);
    return o;
}
PdfObject PdfObject::makeName(std::string name) {
    PdfObject o;
    o.kind = Kind::Name;
    o.text = std::move(name);
    return o;
}
PdfObject PdfObject::makeString(std::string bytes, bool hex) {
    PdfObject o;
    o.kind = Kind::String;
    o.text = std::move(bytes);
    o.hexString = hex;
    return o;
}
PdfObject PdfObject::makeTextString(std::string_view utf8) {
    const bool ascii = std::all_of(utf8.begin(), utf8.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 0x20 && u < 0x7F;
    });
    if (ascii) return makeString(std::string(utf8), false);
    return makeString(utf8ToUtf16Be(utf8, true), true);
}
PdfObject PdfObject::makeRef(PdfRef r) {
    PdfObject o;
    o.kind = Kind::Ref;
    o.ref = r;
    return o;
}
PdfObject PdfObject::makeArray(PdfArray items) {
    PdfObject o;
    o.kind = Kind::Array;
    o.array = std::make_shared<const PdfArray>(std::move(items));
    return o;
}
PdfObject PdfObject::makeDict(PdfDict d) {
    PdfObject o;
    o.kind = Kind::Dict;
    o.dict = std::make_shared<const PdfDict>(std::move(d));
    return o;
}

std::optional<double> PdfObject::number() const {
    if (kind == Kind::Int) return static_cast<double>(integer);
    if (kind != Kind::Real) return std::nullopt;
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
    double value = 0;
    bool any = false;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
        value = value * 10 + (text[i] - '0');
        any = true;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        double scale = 0.1;
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            value += (text[i] - '0') * scale;
            scale *= 0.1;
            any = true;
        }
    }
    if (!any) return std::nullopt;
    return negative ? -value : value;
}

// ---- parser --------------------------------------------------------------------------

bool PdfParser::atEnd() {
    skipWhitespaceAndComments();
    return pos_ >= data_.size();
}

void PdfParser::skipWhitespaceAndComments() {
    while (pos_ < data_.size()) {
        const std::uint8_t c = data_[pos_];
        if (isPdfWhitespace(c)) {
            ++pos_;
        } else if (c == '%') {
            while (pos_ < data_.size() && data_[pos_] != '\n' && data_[pos_] != '\r') ++pos_;
        } else {
            break;
        }
    }
}

std::string_view PdfParser::readKeyword() {
    skipWhitespaceAndComments();
    const std::uint64_t start = pos_;
    while (pos_ < data_.size() && isRegular(data_[pos_])) ++pos_;
    return std::string_view(reinterpret_cast<const char*>(data_.data()) + start, pos_ - start);
}

SignResult<PdfObject> PdfParser::parseObject(int depth) {
    if (depth > kMaxObjectDepth) return fail(SignErrorCode::LimitExceeded, "object nesting too deep");
    skipWhitespaceAndComments();
    if (pos_ >= data_.size()) return bad("unexpected end of data");
    const std::uint8_t c = data_[pos_];
    if (c == '/') return parseName();
    if (c == '(') return parseLiteralString();
    if (c == '<') {
        if (pos_ + 1 < data_.size() && data_[pos_ + 1] == '<') return parseDictOrStream(depth);
        return parseHexString();
    }
    if (c == '[') return parseArray(depth);
    if (isDigit(c) || c == '+' || c == '-' || c == '.') return parseNumberOrRef();
    const std::string_view kw = readKeyword();
    if (kw == "true") return PdfObject::makeBool(true);
    if (kw == "false") return PdfObject::makeBool(false);
    if (kw == "null") return PdfObject::null();
    return bad("unexpected token");
}

SignResult<PdfObject> PdfParser::parseNumberOrRef() {
    const std::uint64_t start = pos_;
    std::size_t i = pos_;
    if (data_[i] == '+' || data_[i] == '-') ++i;
    bool digits = false;
    bool real = false;
    std::size_t digitCount = 0;
    while (i < data_.size() && (isDigit(data_[i]) || data_[i] == '.')) {
        if (data_[i] == '.') {
            if (real) break;
            real = true;
        } else {
            digits = true;
            ++digitCount;
        }
        ++i;
    }
    if (!digits) return bad("malformed number");
    if (i < data_.size() && isRegular(data_[i])) return bad("malformed number");
    pos_ = i;
    const std::string lexeme(reinterpret_cast<const char*>(data_.data()) + start, i - start);
    if (real) return PdfObject::makeReal(lexeme);

    // Integer; overflow degrades to a Real lexeme (never an error).
    std::int64_t value = 0;
    bool overflow = digitCount > 18;
    if (!overflow) {
        std::size_t k = (lexeme[0] == '+' || lexeme[0] == '-') ? 1 : 0;
        for (; k < lexeme.size(); ++k) value = value * 10 + (lexeme[k] - '0');
        if (lexeme[0] == '-') value = -value;
    }
    if (overflow) return PdfObject::makeReal(lexeme);

    // Reference lookahead: "<uint> <uint> R".
    if (value >= 0 && lexeme[0] != '+' && lexeme[0] != '-' && value <= 0x7FFFFFFF) {
        std::size_t j = pos_;
        const auto skipWs = [&] {
            while (j < data_.size() && isPdfWhitespace(data_[j])) ++j;
        };
        skipWs();
        const std::size_t genStart = j;
        std::uint64_t gen = 0;
        while (j < data_.size() && isDigit(data_[j]) && j - genStart < 6) gen = gen * 10 + (data_[j++] - '0');
        if (j > genStart && gen <= 65535 && j < data_.size() && isPdfWhitespace(data_[j])) {
            skipWs();
            if (j < data_.size() && data_[j] == 'R' && (j + 1 >= data_.size() || !isRegular(data_[j + 1]))) {
                pos_ = j + 1;
                return PdfObject::makeRef({static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(gen)});
            }
        }
    }
    return PdfObject::makeInt(value);
}

SignResult<PdfObject> PdfParser::parseName() {
    ++pos_; // '/'
    std::string name;
    while (pos_ < data_.size() && isRegular(data_[pos_])) {
        std::uint8_t c = data_[pos_++];
        if (c == '#' && pos_ + 1 < data_.size()) {
            const int h = hexValue(data_[pos_]);
            const int l = hexValue(data_[pos_ + 1]);
            if (h >= 0 && l >= 0) {
                c = static_cast<std::uint8_t>(h * 16 + l);
                pos_ += 2;
            }
        }
        name.push_back(static_cast<char>(c));
        if (name.size() > 4096) return fail(SignErrorCode::LimitExceeded, "name too long");
    }
    return PdfObject::makeName(std::move(name));
}

SignResult<PdfObject> PdfParser::parseLiteralString() {
    const std::uint64_t begin = pos_;
    ++pos_;
    std::string out;
    int nesting = 1;
    while (pos_ < data_.size()) {
        std::uint8_t c = data_[pos_++];
        if (c == '(') {
            ++nesting;
            if (nesting > 256) return fail(SignErrorCode::LimitExceeded, "string nesting too deep");
        } else if (c == ')') {
            if (--nesting == 0) {
                PdfObject o = PdfObject::makeString(std::move(out), false);
                o.srcBegin = begin;
                o.srcEnd = pos_;
                return o;
            }
        } else if (c == '\\') {
            if (pos_ >= data_.size()) break;
            c = data_[pos_++];
            switch (c) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '\r':
                if (pos_ < data_.size() && data_[pos_] == '\n') ++pos_;
                continue;
            case '\n': continue;
            default:
                if (c >= '0' && c <= '7') {
                    unsigned v = c - '0';
                    for (int k = 0; k < 2 && pos_ < data_.size() && data_[pos_] >= '0' && data_[pos_] <= '7'; ++k)
                        v = v * 8 + (data_[pos_++] - '0');
                    c = static_cast<std::uint8_t>(v & 0xFF);
                }
                break;
            }
        }
        out.push_back(static_cast<char>(c));
        if (out.size() > kMaxStringBytes) return fail(SignErrorCode::LimitExceeded, "string too long");
    }
    return bad("unterminated string");
}

SignResult<PdfObject> PdfParser::parseHexString() {
    const std::uint64_t begin = pos_;
    ++pos_;
    std::string out;
    int pending = -1;
    while (pos_ < data_.size()) {
        const std::uint8_t c = data_[pos_++];
        if (c == '>') {
            if (pending >= 0) out.push_back(static_cast<char>(pending * 16));
            PdfObject o = PdfObject::makeString(std::move(out), true);
            o.srcBegin = begin;
            o.srcEnd = pos_;
            return o;
        }
        if (isPdfWhitespace(c)) continue;
        const int v = hexValue(c);
        if (v < 0) return bad("invalid hex string");
        if (pending < 0) {
            pending = v;
        } else {
            out.push_back(static_cast<char>(pending * 16 + v));
            pending = -1;
            if (out.size() > kMaxStringBytes) return fail(SignErrorCode::LimitExceeded, "string too long");
        }
    }
    return bad("unterminated hex string");
}

SignResult<PdfObject> PdfParser::parseArray(int depth) {
    ++pos_;
    PdfArray items;
    for (;;) {
        skipWhitespaceAndComments();
        if (pos_ >= data_.size()) return bad("unterminated array");
        if (data_[pos_] == ']') {
            ++pos_;
            return PdfObject::makeArray(std::move(items));
        }
        if (items.size() >= kMaxArrayLength) return fail(SignErrorCode::LimitExceeded, "array too long");
        auto item = parseObject(depth + 1);
        if (!item) return item;
        items.push_back(std::move(*item));
    }
}

SignResult<PdfObject> PdfParser::parseDictOrStream(int depth) {
    pos_ += 2;
    PdfDict dict;
    for (;;) {
        skipWhitespaceAndComments();
        if (pos_ >= data_.size()) return bad("unterminated dictionary");
        if (data_[pos_] == '>') {
            if (pos_ + 1 < data_.size() && data_[pos_ + 1] == '>') {
                pos_ += 2;
                return PdfObject::makeDict(std::move(dict));
            }
            return bad("malformed dictionary end");
        }
        if (data_[pos_] != '/') return bad("dictionary key is not a name");
        auto key = parseName();
        if (!key) return std::unexpected(key.error());
        if (dict.entries.size() >= kMaxDictEntries) return fail(SignErrorCode::LimitExceeded, "dictionary too large");
        skipWhitespaceAndComments();
        if (pos_ < data_.size() && data_[pos_] == '>') return bad("dictionary key without value");
        auto value = parseObject(depth + 1);
        if (!value) return value;
        auto [it, inserted] = dict.entries.emplace(key->text, std::move(*value));
        if (!inserted) dict.duplicateKeys = true;
        (void)it;
    }
}

// ---- indirect objects ---------------------------------------------------------------------

namespace {

bool matchAt(ByteView data, std::uint64_t pos, std::string_view word) {
    if (pos > data.size() || data.size() - pos < word.size()) return false;
    return std::memcmp(data.data() + pos, word.data(), word.size()) == 0;
}

} // namespace

SignResult<IndirectObject> parseIndirectObjectAt(ByteView data, std::uint64_t offset, const LengthResolver* lengths) {
    if (offset >= data.size()) return bad("object offset out of range");
    PdfParser p(data, offset);
    IndirectObject io;
    io.begin = offset;
    p.skipWhitespaceAndComments();
    io.begin = p.position();
    auto num = p.parseObject();
    auto gen = num ? p.parseObject() : num;
    if (!num || !gen || !num->isInt() || !gen->isInt() || num->integer < 0 || num->integer > 0x7FFFFFFF ||
        gen->integer < 0 || gen->integer > 65535)
        return bad("malformed object header");
    if (p.readKeyword() != "obj") return bad("missing obj keyword");
    io.id = {static_cast<std::uint32_t>(num->integer), static_cast<std::uint32_t>(gen->integer)};
    auto value = p.parseObject();
    if (!value) return std::unexpected(value.error());
    io.value = std::move(*value);

    const std::uint64_t afterValue = p.position();
    const std::string_view kw = p.readKeyword();
    if (kw == "endobj") {
        io.end = p.position();
        return io;
    }
    if (kw != "stream" || !io.value.isDict()) {
        // Lenient: a missing endobj ends the object at the value.
        io.end = afterValue;
        return io;
    }
    std::uint64_t pos = p.position();
    if (matchAt(data, pos, "\r\n")) pos += 2;
    else if (matchAt(data, pos, "\n")) pos += 1;
    else if (matchAt(data, pos, "\r")) pos += 1;
    else return bad("stream keyword not followed by an end-of-line");
    io.isStream = true;
    io.streamBegin = pos;

    std::optional<std::int64_t> length;
    if (const PdfObject* l = io.value.dict->find("Length")) {
        if (l->isInt()) length = l->integer;
        else if (l->isRef() && lengths && *lengths) length = (*lengths)(l->ref);
    }
    bool framed = false;
    if (length && *length >= 0 && static_cast<std::uint64_t>(*length) <= data.size() - pos) {
        PdfParser q(data, pos + static_cast<std::uint64_t>(*length));
        if (q.readKeyword() == "endstream") {
            io.streamLength = static_cast<std::uint64_t>(*length);
            p.seek(q.position());
            framed = true;
        }
    }
    if (!framed) {
        static constexpr std::string_view kEnd = "endstream";
        const auto* begin = data.data() + pos;
        const auto* end = data.data() + data.size();
        const auto* hit = std::search(begin, end, kEnd.begin(), kEnd.end());
        if (hit == end) return bad("stream without endstream");
        std::uint64_t dataEnd = static_cast<std::uint64_t>(hit - data.data());
        if (dataEnd > pos && data[dataEnd - 1] == '\n') --dataEnd;
        if (dataEnd > pos && data[dataEnd - 1] == '\r') --dataEnd;
        io.streamLength = dataEnd - pos;
        p.seek(static_cast<std::uint64_t>(hit - data.data()) + kEnd.size());
    }
    const std::uint64_t afterStream = p.position();
    const std::string_view tail = p.readKeyword();
    io.end = tail == "endobj" ? p.position() : afterStream;
    return io;
}

// ---- writer ------------------------------------------------------------------------------------

void appendName(std::string& out, std::string_view name) {
    out.push_back('/');
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (const char ch : name) {
        const auto c = static_cast<std::uint8_t>(ch);
        if (c <= 32 || c >= 127 || c == '#' || isPdfDelimiter(c)) {
            out.push_back('#');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        } else {
            out.push_back(ch);
        }
    }
}

void appendLiteralString(std::string& out, std::string_view bytes) {
    out.push_back('(');
    for (const char ch : bytes) {
        const auto c = static_cast<std::uint8_t>(ch);
        if (c == '(' || c == ')' || c == '\\') {
            out.push_back('\\');
            out.push_back(ch);
        } else if (c < 0x20 || c >= 0x7F) {
            out.push_back('\\');
            out.push_back(static_cast<char>('0' + ((c >> 6) & 7)));
            out.push_back(static_cast<char>('0' + ((c >> 3) & 7)));
            out.push_back(static_cast<char>('0' + (c & 7)));
        } else {
            out.push_back(ch);
        }
    }
    out.push_back(')');
}

void serialize(const PdfObject& o, std::string& out) {
    switch (o.kind) {
    case PdfObject::Kind::Null: out += "null"; break;
    case PdfObject::Kind::Bool: out += o.boolean ? "true" : "false"; break;
    case PdfObject::Kind::Int: out += std::to_string(o.integer); break;
    case PdfObject::Kind::Real: out += o.text; break;
    case PdfObject::Kind::Name: appendName(out, o.text); break;
    case PdfObject::Kind::String:
        if (o.hexString) {
            static constexpr char kHex[] = "0123456789ABCDEF";
            out.push_back('<');
            for (const char ch : o.text) {
                const auto c = static_cast<std::uint8_t>(ch);
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 15]);
            }
            out.push_back('>');
        } else {
            appendLiteralString(out, o.text);
        }
        break;
    case PdfObject::Kind::Array: {
        out.push_back('[');
        bool first = true;
        if (o.array)
            for (const PdfObject& item : *o.array) {
                if (!first) out.push_back(' ');
                first = false;
                serialize(item, out);
            }
        out.push_back(']');
        break;
    }
    case PdfObject::Kind::Dict: {
        out += "<<";
        if (o.dict)
            for (const auto& [key, value] : o.dict->entries) {
                appendName(out, key);
                out.push_back(' ');
                serialize(value, out);
                out.push_back(' ');
            }
        out += ">>";
        break;
    }
    case PdfObject::Kind::Ref:
        out += std::to_string(o.ref.num);
        out.push_back(' ');
        out += std::to_string(o.ref.gen);
        out += " R";
        break;
    }
}

std::string serialize(const PdfObject& object) {
    std::string out;
    serialize(object, out);
    return out;
}

PdfObject dictOf(std::initializer_list<std::pair<std::string, PdfObject>> entries) {
    PdfDict d;
    for (const auto& [k, v] : entries) d.entries[k] = v;
    return PdfObject::makeDict(std::move(d));
}

std::string utf8ToUtf16Be(std::string_view s, bool bom) {
    std::string out;
    const auto put = [&](unsigned unit) {
        out.push_back(static_cast<char>((unit >> 8) & 0xFF));
        out.push_back(static_cast<char>(unit & 0xFF));
    };
    if (bom) put(0xFEFF);
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<std::uint8_t>(s[i]);
        unsigned cp = 0xFFFD;
        std::size_t len = 1;
        if (b0 < 0x80) {
            cp = b0;
        } else if (b0 >= 0xC2 && b0 <= 0xDF && i + 1 < s.size() && (static_cast<std::uint8_t>(s[i + 1]) & 0xC0) == 0x80) {
            cp = ((b0 & 0x1Fu) << 6) | (static_cast<std::uint8_t>(s[i + 1]) & 0x3Fu);
            len = 2;
        } else if (b0 >= 0xE0 && b0 <= 0xEF && i + 2 < s.size() &&
                   (static_cast<std::uint8_t>(s[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<std::uint8_t>(s[i + 2]) & 0xC0) == 0x80) {
            cp = ((b0 & 0x0Fu) << 12) | ((static_cast<std::uint8_t>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<std::uint8_t>(s[i + 2]) & 0x3Fu);
            len = 3;
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        } else if (b0 >= 0xF0 && b0 <= 0xF4 && i + 3 < s.size() &&
                   (static_cast<std::uint8_t>(s[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<std::uint8_t>(s[i + 2]) & 0xC0) == 0x80 &&
                   (static_cast<std::uint8_t>(s[i + 3]) & 0xC0) == 0x80) {
            cp = ((b0 & 0x07u) << 18) | ((static_cast<std::uint8_t>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<std::uint8_t>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<std::uint8_t>(s[i + 3]) & 0x3Fu);
            len = 4;
            if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            put(0xD800 + (cp >> 10));
            put(0xDC00 + (cp & 0x3FF));
        } else {
            put(cp);
        }
    }
    return out;
}

} // namespace rivet::pdf::sign
