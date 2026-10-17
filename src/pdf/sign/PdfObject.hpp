// SPDX-License-Identifier: MPL-2.0
#pragma once

// Minimal, bounded PDF object syntax reader and writer for the signing
// engine (ADR-0037). This is NOT a content parser: it understands only the
// object syntax (null/bool/number/name/string/array/dict/reference/stream
// framing) needed to navigate trailer, xref, catalog, AcroForm, page and
// signature objects and to re-serialize the few objects an incremental
// update rewrites. Every input is untrusted: all loops are bounded, all
// offsets are 64-bit with overflow checks, nesting is limited.

#include "pdf/sign/SignError.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::pdf::sign {

using ByteView = std::span<const std::uint8_t>;
using Bytes = std::vector<std::uint8_t>;

inline constexpr int kMaxObjectDepth = 48;
inline constexpr std::size_t kMaxArrayLength = std::size_t{1} << 22;
inline constexpr std::size_t kMaxDictEntries = std::size_t{1} << 16;
inline constexpr std::size_t kMaxStringBytes = std::size_t{1} << 26; // 64 MiB (a hex /Contents can be large)

struct PdfRef {
    std::uint32_t num = 0;
    std::uint32_t gen = 0;
    bool operator==(const PdfRef&) const = default;
    auto operator<=>(const PdfRef&) const = default;
};

class PdfObject;
using PdfArray = std::vector<PdfObject>;
struct PdfDict;

class PdfObject {
public:
    enum class Kind : std::uint8_t { Null, Bool, Int, Real, Name, String, Array, Dict, Ref };

    Kind kind = Kind::Null;
    bool boolean = false;
    std::int64_t integer = 0;
    // Name (decoded, without '/'), Real (original lexeme) or String (decoded bytes).
    std::string text;
    bool hexString = false;
    // Absolute file offsets of the string token ('<' / '(' through '>' / ')')
    // when it was parsed from a file buffer (0/0 otherwise).
    std::uint64_t srcBegin = 0;
    std::uint64_t srcEnd = 0;
    PdfRef ref;
    std::shared_ptr<const PdfArray> array;
    std::shared_ptr<const PdfDict> dict;

    static PdfObject null() { return {}; }
    static PdfObject makeBool(bool v);
    static PdfObject makeInt(std::int64_t v);
    static PdfObject makeReal(std::string lexeme);
    static PdfObject makeName(std::string name);
    static PdfObject makeString(std::string bytes, bool hex = false);
    // Text string: PDFDocEncoding-compatible ASCII as a literal, anything
    // else as UTF-16BE with a BOM (hex).
    static PdfObject makeTextString(std::string_view utf8);
    static PdfObject makeRef(PdfRef r);
    static PdfObject makeArray(PdfArray items);
    static PdfObject makeDict(PdfDict d);

    bool isNull() const { return kind == Kind::Null; }
    bool isInt() const { return kind == Kind::Int; }
    bool isNumber() const { return kind == Kind::Int || kind == Kind::Real; }
    bool isName() const { return kind == Kind::Name; }
    bool isName(std::string_view n) const { return kind == Kind::Name && text == n; }
    bool isString() const { return kind == Kind::String; }
    bool isArray() const { return kind == Kind::Array && array != nullptr; }
    bool isDict() const { return kind == Kind::Dict && dict != nullptr; }
    bool isRef() const { return kind == Kind::Ref; }
    // Numeric value of an Int or Real (Real is parsed from its lexeme).
    std::optional<double> number() const;
};

struct PdfDict {
    std::map<std::string, PdfObject> entries;
    bool duplicateKeys = false; // a key appeared more than once while parsing

    const PdfObject* find(std::string_view key) const {
        auto it = entries.find(std::string(key));
        return it == entries.end() ? nullptr : &it->second;
    }
    void set(std::string key, PdfObject value) { entries[std::move(key)] = std::move(value); }
    void erase(std::string_view key) { entries.erase(std::string(key)); }
    bool isType(std::string_view type) const {
        const PdfObject* t = find("Type");
        return t && t->isName(type);
    }
};

// Parses one object at `pos` of `data` (references "n g R" are recognised).
// `pos` is advanced past the object. Offsets in String tokens are absolute
// within `data`.
class PdfParser {
public:
    explicit PdfParser(ByteView data, std::uint64_t pos = 0) : data_(data), pos_(pos) {}

    SignResult<PdfObject> parseObject(int depth = 0);
    std::uint64_t position() const { return pos_; }
    void seek(std::uint64_t pos) { pos_ = pos; }
    bool atEnd();
    void skipWhitespaceAndComments();
    // Reads a bare keyword (run of regular characters); empty at a delimiter/end.
    std::string_view readKeyword();
    ByteView data() const { return data_; }

private:
    SignResult<PdfObject> parseNumberOrRef();
    SignResult<PdfObject> parseName();
    SignResult<PdfObject> parseLiteralString();
    SignResult<PdfObject> parseHexString();
    SignResult<PdfObject> parseArray(int depth);
    SignResult<PdfObject> parseDictOrStream(int depth);

    ByteView data_;
    std::uint64_t pos_ = 0;
};

struct IndirectObject {
    PdfRef id;
    PdfObject value;
    std::uint64_t begin = 0;        // offset of "n g obj"
    std::uint64_t end = 0;          // offset after "endobj" (or after the stream when endobj is missing)
    bool isStream = false;
    std::uint64_t streamBegin = 0;  // first data byte
    std::uint64_t streamLength = 0; // raw (encoded) length
};

// Callback resolving an indirect /Length (a direct integer object), or nullopt.
using LengthResolver = std::function<std::optional<std::int64_t>(PdfRef)>;

// Parses "n g obj <value> [stream ... endstream] endobj" at `offset`.
SignResult<IndirectObject> parseIndirectObjectAt(ByteView data, std::uint64_t offset,
                                                 const LengthResolver* lengths = nullptr);

// ---- writer -------------------------------------------------------------

// Appends the PDF serialization of `object` to `out`. Strings keep their
// bytes (hex strings stay hex); names are re-escaped; reals keep their lexeme.
void serialize(const PdfObject& object, std::string& out);
std::string serialize(const PdfObject& object);
void appendName(std::string& out, std::string_view name);
// Literal string with escaping (bytes outside printable ASCII become \ddd).
void appendLiteralString(std::string& out, std::string_view bytes);

// Convenience for building dictionaries: entries are "/Key value" pairs.
PdfObject dictOf(std::initializer_list<std::pair<std::string, PdfObject>> entries);

// Text helpers.
std::string utf8ToUtf16Be(std::string_view utf8, bool bom);
inline bool isPdfWhitespace(std::uint8_t c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
inline bool isPdfDelimiter(std::uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
           c == '%';
}

} // namespace rivet::pdf::sign
