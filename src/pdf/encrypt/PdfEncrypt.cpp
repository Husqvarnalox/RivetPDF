// SPDX-License-Identifier: MPL-2.0
#include "pdf/encrypt/PdfEncrypt.hpp"

#include "crypto/Providers.hpp"
#include "pdf/encrypt/StandardHandler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string_view>
#include <utility>

// Tokenizer-level scanner and writer for the constrained PDFium save format
// (ADR-0046). It is NOT a general PDF parser: it understands tokens (names,
// strings, numbers, keywords, array/dictionary brackets), the classic xref
// table + trailer, and "n g obj ... [stream ... endstream] endobj" framing,
// which is all that is needed to re-emit every object with its strings and
// streams encrypted. Every input byte is untrusted; every loop advances.

namespace rivet::pdf::encrypt {

namespace {

constexpr const char* kSubsystem = "pdf.encrypt";
constexpr std::size_t kMaxFileSize = std::size_t{1} << 31; // 2 GiB
constexpr int kMaxDepth = 64;
constexpr std::size_t kMaxObjectTokens = std::size_t{1} << 24;

core::Error invalidDoc(std::string m) {
    return core::makeError(core::ErrorCode::InvalidDocument, std::move(m), kSubsystem);
}
core::Error unsupported(std::string m) {
    return core::makeError(core::ErrorCode::Unsupported, std::move(m), kSubsystem);
}
std::unexpected<core::Error> failDoc(std::string m) { return std::unexpected(invalidDoc(std::move(m))); }

bool isWs(std::uint8_t c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool isDelim(std::uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
           c == '%';
}
bool isRegular(std::uint8_t c) { return !isWs(c) && !isDelim(c); }
int hexVal(std::uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

enum class T : std::uint8_t { Keyword, Name, Literal, Hex, ArrOpen, ArrClose, DictOpen, DictClose };

struct Tok {
    T kind = T::Keyword;
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string text; // keyword lexeme, decoded name, decoded string bytes
};

class Lexer {
public:
    explicit Lexer(ByteView d, std::size_t pos = 0) : d_(d), pos_(pos) {}
    std::size_t pos() const { return pos_; }
    void seek(std::size_t p) { pos_ = p; }

    // nullopt at end of data; error on malformed syntax.
    core::Result<std::optional<Tok>> next() {
        skipSpace();
        if (pos_ >= d_.size()) return std::optional<Tok>{};
        Tok t;
        t.begin = pos_;
        const std::uint8_t c = d_[pos_];
        switch (c) {
        case '(': {
            auto r = literal(t);
            if (!r) return std::unexpected(r.error());
            break;
        }
        case '<':
            if (pos_ + 1 < d_.size() && d_[pos_ + 1] == '<') {
                t.kind = T::DictOpen;
                pos_ += 2;
            } else {
                auto r = hex(t);
                if (!r) return std::unexpected(r.error());
            }
            break;
        case '>':
            if (pos_ + 1 < d_.size() && d_[pos_ + 1] == '>') {
                t.kind = T::DictClose;
                pos_ += 2;
            } else {
                return failDoc("stray '>' in object syntax");
            }
            break;
        case '[': t.kind = T::ArrOpen; ++pos_; break;
        case ']': t.kind = T::ArrClose; ++pos_; break;
        case '/': {
            t.kind = T::Name;
            ++pos_;
            while (pos_ < d_.size() && isRegular(d_[pos_])) {
                if (d_[pos_] == '#' && pos_ + 2 < d_.size() && hexVal(d_[pos_ + 1]) >= 0 && hexVal(d_[pos_ + 2]) >= 0) {
                    t.text += static_cast<char>(hexVal(d_[pos_ + 1]) * 16 + hexVal(d_[pos_ + 2]));
                    pos_ += 3;
                } else {
                    t.text += static_cast<char>(d_[pos_++]);
                }
            }
            break;
        }
        case ')': case '{': case '}':
            return failDoc("unexpected delimiter in object syntax");
        default:
            t.kind = T::Keyword;
            while (pos_ < d_.size() && isRegular(d_[pos_])) t.text += static_cast<char>(d_[pos_++]);
            break;
        }
        t.end = pos_;
        return std::optional<Tok>(std::move(t));
    }

private:
    void skipSpace() {
        while (pos_ < d_.size()) {
            if (isWs(d_[pos_])) {
                ++pos_;
            } else if (d_[pos_] == '%') {
                while (pos_ < d_.size() && d_[pos_] != '\n' && d_[pos_] != '\r') ++pos_;
            } else {
                break;
            }
        }
    }

    core::Status literal(Tok& t) {
        t.kind = T::Literal;
        ++pos_; // '('
        int depth = 1;
        while (pos_ < d_.size()) {
            const std::uint8_t c = d_[pos_++];
            if (c == '\\') {
                if (pos_ >= d_.size()) break;
                const std::uint8_t e = d_[pos_++];
                switch (e) {
                case 'n': t.text += '\n'; break;
                case 'r': t.text += '\r'; break;
                case 't': t.text += '\t'; break;
                case 'b': t.text += '\b'; break;
                case 'f': t.text += '\f'; break;
                case '\r':
                    if (pos_ < d_.size() && d_[pos_] == '\n') ++pos_;
                    break;
                case '\n': break;
                default:
                    if (e >= '0' && e <= '7') {
                        unsigned v = e - '0';
                        for (int k = 0; k < 2 && pos_ < d_.size() && d_[pos_] >= '0' && d_[pos_] <= '7'; ++k)
                            v = v * 8 + (d_[pos_++] - '0');
                        t.text += static_cast<char>(v & 0xFF);
                    } else {
                        t.text += static_cast<char>(e);
                    }
                }
            } else if (c == '(') {
                ++depth;
                t.text += '(';
            } else if (c == ')') {
                if (--depth == 0) return core::ok();
                t.text += ')';
            } else if (c == '\r') {
                if (pos_ < d_.size() && d_[pos_] == '\n') ++pos_;
                t.text += '\n';
            } else {
                t.text += static_cast<char>(c);
            }
        }
        return std::unexpected(invalidDoc("unterminated literal string"));
    }

    core::Status hex(Tok& t) {
        t.kind = T::Hex;
        ++pos_; // '<'
        int pending = -1;
        while (pos_ < d_.size()) {
            const std::uint8_t c = d_[pos_++];
            if (c == '>') {
                if (pending >= 0) t.text += static_cast<char>(pending * 16);
                return core::ok();
            }
            if (isWs(c)) continue;
            const int v = hexVal(c);
            if (v < 0) return std::unexpected(invalidDoc("invalid character in hex string"));
            if (pending < 0) {
                pending = v;
            } else {
                t.text += static_cast<char>(pending * 16 + v);
                pending = -1;
            }
        }
        return std::unexpected(invalidDoc("unterminated hex string"));
    }

    ByteView d_;
    std::size_t pos_;
};

bool parseU64(std::string_view s, std::uint64_t& out) {
    if (s.empty() || s.size() > 18) return false;
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = v;
    return true;
}

bool isKeyword(const Tok& t, std::string_view kw) { return t.kind == T::Keyword && t.text == kw; }

// Index just past the value starting at token i (-1 when malformed).
std::size_t skipValue(const std::vector<Tok>& t, std::size_t i) {
    if (i >= t.size()) return static_cast<std::size_t>(-1);
    if (t[i].kind == T::ArrOpen || t[i].kind == T::DictOpen) {
        int depth = 0;
        for (; i < t.size(); ++i) {
            if (t[i].kind == T::ArrOpen || t[i].kind == T::DictOpen) ++depth;
            if (t[i].kind == T::ArrClose || t[i].kind == T::DictClose) {
                if (--depth == 0) return i + 1;
            }
        }
        return static_cast<std::size_t>(-1);
    }
    std::uint64_t dummy = 0;
    if (t[i].kind == T::Keyword && parseU64(t[i].text, dummy) && i + 2 < t.size() && t[i + 1].kind == T::Keyword &&
        parseU64(t[i + 1].text, dummy) && isKeyword(t[i + 2], "R"))
        return i + 3;
    return i + 1;
}

struct DictEntry {
    std::string key;
    std::size_t keyTok = 0;
    std::size_t valBegin = 0; // token indices [valBegin, valEnd)
    std::size_t valEnd = 0;
};

// Top-level entries of the dictionary opening at t[open].
core::Result<std::vector<DictEntry>> topEntries(const std::vector<Tok>& t, std::size_t open) {
    std::vector<DictEntry> out;
    std::size_t i = open + 1;
    while (i < t.size() && t[i].kind != T::DictClose) {
        if (t[i].kind != T::Name) return failDoc("dictionary key is not a name");
        DictEntry e;
        e.key = t[i].text;
        e.keyTok = i;
        e.valBegin = i + 1;
        e.valEnd = skipValue(t, e.valBegin);
        if (e.valEnd == static_cast<std::size_t>(-1)) return failDoc("truncated dictionary value");
        if (e.valEnd > t.size()) return failDoc("truncated dictionary value");
        out.push_back(std::move(e));
        i = out.back().valEnd;
    }
    if (i >= t.size()) return failDoc("unterminated dictionary");
    return out;
}

const DictEntry* findEntry(const std::vector<DictEntry>& e, std::string_view key) {
    for (const auto& x : e)
        if (x.key == key) return &x;
    return nullptr;
}

// Reads tokens of one dictionary starting at its DictOpen (already consumed
// and passed in `out[0]`) until the matching close.
core::Status readDictTokens(Lexer& lx, std::vector<Tok>& out) {
    int depth = 1;
    while (depth > 0) {
        auto n = lx.next();
        if (!n) return std::unexpected(n.error());
        if (!*n) return std::unexpected(invalidDoc("unterminated dictionary"));
        if ((*n)->kind == T::DictOpen || (*n)->kind == T::ArrOpen) {
            if (++depth > kMaxDepth) return std::unexpected(invalidDoc("object nesting too deep"));
        } else if ((*n)->kind == T::DictClose || (*n)->kind == T::ArrClose) {
            --depth;
        }
        out.push_back(std::move(**n));
        if (out.size() > kMaxObjectTokens) return std::unexpected(invalidDoc("object too large"));
    }
    return core::ok();
}

struct XrefObject {
    std::uint32_t num = 0;
    std::uint32_t gen = 0;
    std::uint64_t offset = 0;
};

struct Out {
    Bytes bytes;
    void put(std::string_view s) { bytes.insert(bytes.end(), s.begin(), s.end()); }
    void put(ByteView s) { bytes.insert(bytes.end(), s.begin(), s.end()); }
    void put(const std::uint8_t* p, std::size_t n) { bytes.insert(bytes.end(), p, p + n); }
};

std::string hex(ByteView data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "<";
    for (const std::uint8_t b : data) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0xF];
    }
    out += '>';
    return out;
}

struct Substitution {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string text;
};

class Encryptor {
public:
    Encryptor(ByteView d, const EncryptOptions& o) : d_(d), options_(o) {}

    core::Result<Bytes> run() {
        if (d_.size() < 16 || std::memcmp(d_.data(), "%PDF-", 5) != 0) return failDoc("not a PDF file (missing header)");
        if (d_.size() > kMaxFileSize) return failDoc("file too large");
        if (auto s = readVersion(); !s) return std::unexpected(s.error());
        if (auto s = readXref(); !s) return std::unexpected(s.error());
        if (auto s = readTrailer(); !s) return std::unexpected(s.error());

        auto setup = buildSetup(options_, id0_);
        if (!setup) return std::unexpected(setup.error());
        setup_ = std::move(*setup);

        Out out;
        writeHeader(out);
        std::map<std::uint32_t, std::uint64_t> newOffsets;
        for (const auto& obj : objects_) {
            newOffsets[obj.num] = out.bytes.size();
            if (auto s = writeObject(obj, out); !s) return std::unexpected(s.error());
        }

        const std::uint32_t encryptNum = size_;
        newOffsets[encryptNum] = out.bytes.size();
        out.put(std::to_string(encryptNum) + " 0 obj\n" + encryptDictionary(setup_) + "\nendobj\n");
        writeXrefAndTrailer(out, newOffsets, encryptNum);
        return std::move(out.bytes);
    }

private:
    // ---- header ----
    core::Status readVersion() {
        if (!(d_[5] >= '1' && d_[5] <= '2' && d_[6] == '.' && d_[7] >= '0' && d_[7] <= '9'))
            return std::unexpected(invalidDoc("unreadable PDF version"));
        major_ = d_[5] - '0';
        minor_ = d_[7] - '0';
        return core::ok();
    }

    void writeHeader(Out& out) {
        int wantMajor = options_.cipher == Cipher::Aes256 ? 2 : 1;
        int wantMinor = options_.cipher == Cipher::Aes256 ? 0 : 6;
        if (std::pair(major_, minor_) > std::pair(wantMajor, wantMinor)) {
            wantMajor = major_;
            wantMinor = minor_;
        }
        // major_/minor_ are single digits (validated in readVersion), so build the line directly.
        const char line[] = {'%', 'P', 'D', 'F', '-', static_cast<char>('0' + wantMajor % 10), '.',
                             static_cast<char>('0' + wantMinor % 10)};
        out.put(std::string_view(line, sizeof line));
        // Keep whatever followed the version on the first line and the binary-marker comment line.
        std::size_t p = 8;
        const std::size_t firstObj = objects_.empty() ? 8 : std::min<std::size_t>(objects_.front().offset, d_.size());
        if (firstObj > p) out.put(d_.data() + p, firstObj - p);
    }

    // ---- xref ----
    core::Status readXref() {
        const std::size_t tailBegin = d_.size() > 2048 ? d_.size() - 2048 : 0;
        const std::string_view tail(reinterpret_cast<const char*>(d_.data()) + tailBegin, d_.size() - tailBegin);
        const std::size_t sx = tail.rfind("startxref");
        if (sx == std::string_view::npos) return std::unexpected(invalidDoc("no startxref"));
        Lexer lx(d_, tailBegin + sx + 9);
        auto n = lx.next();
        if (!n) return std::unexpected(n.error());
        std::uint64_t xrefOff = 0;
        if (!*n || !parseU64((*n)->text, xrefOff) || xrefOff >= d_.size()) return std::unexpected(invalidDoc("bad startxref offset"));

        lx.seek(static_cast<std::size_t>(xrefOff));
        auto kw = lx.next();
        if (!kw) return std::unexpected(kw.error());
        if (!*kw || !isKeyword(**kw, "xref")) {
            // An xref stream ("n g obj << /Type /XRef") means a PDF 1.5+ file with compressed structure.
            return std::unexpected(unsupported("cross-reference streams / object streams are not supported (expected a classic xref table)"));
        }
        std::map<std::uint32_t, XrefObject> inUse;
        std::uint64_t maxNum = 0;
        for (;;) {
            auto a = lx.next();
            if (!a) return std::unexpected(a.error());
            if (!*a) return std::unexpected(invalidDoc("truncated xref table"));
            if (isKeyword(**a, "trailer")) break;
            auto b = lx.next();
            if (!b) return std::unexpected(b.error());
            std::uint64_t first = 0;
            std::uint64_t count = 0;
            if (!*b || !parseU64((*a)->text, first) || !parseU64((*b)->text, count) || count > (d_.size() / 10) + 1 ||
                first > 0xFFFFFFFFull)
                return std::unexpected(invalidDoc("bad xref subsection"));
            for (std::uint64_t i = 0; i < count; ++i) {
                auto off = lx.next();
                auto gen = lx.next();
                auto kind = lx.next();
                if (!off) return std::unexpected(off.error());
                if (!gen) return std::unexpected(gen.error());
                if (!kind) return std::unexpected(kind.error());
                std::uint64_t o = 0;
                std::uint64_t g = 0;
                if (!*off || !*gen || !*kind || !parseU64((*off)->text, o) || !parseU64((*gen)->text, g) || g > 65535)
                    return std::unexpected(invalidDoc("bad xref entry"));
                const std::uint64_t num = first + i;
                if (num > 0xFFFFFFFEull) return std::unexpected(invalidDoc("object number out of range"));
                maxNum = std::max(maxNum, num);
                if (isKeyword(**kind, "n")) {
                    if (o == 0 || o >= d_.size()) return std::unexpected(invalidDoc("xref offset out of range"));
                    inUse[static_cast<std::uint32_t>(num)] = {static_cast<std::uint32_t>(num), static_cast<std::uint32_t>(g), o};
                } else if (!isKeyword(**kind, "f")) {
                    return std::unexpected(invalidDoc("bad xref entry type"));
                }
            }
        }
        for (const auto& [num, obj] : inUse) objects_.push_back(obj);
        std::sort(objects_.begin(), objects_.end(), [](const XrefObject& a, const XrefObject& b) { return a.offset < b.offset; });
        for (const auto& obj : objects_) offsetByNum_[obj.num] = obj.offset;
        maxNum_ = maxNum;
        trailerLexer_ = lx.pos();
        return core::ok();
    }

    // ---- trailer ----
    core::Status readTrailer() {
        Lexer lx(d_, trailerLexer_);
        auto open = lx.next();
        if (!open) return std::unexpected(open.error());
        if (!*open || (*open)->kind != T::DictOpen) return std::unexpected(invalidDoc("trailer dictionary missing"));
        trailer_.clear();
        trailer_.push_back(std::move(**open));
        if (auto s = readDictTokens(lx, trailer_); !s) return s;
        auto entries = topEntries(trailer_, 0);
        if (!entries) return std::unexpected(entries.error());
        trailerEntries_ = std::move(*entries);

        if (findEntry(trailerEntries_, "Encrypt")) return std::unexpected(unsupported("the document is already encrypted (re-encryption is not supported)"));
        if (findEntry(trailerEntries_, "Prev") || findEntry(trailerEntries_, "XRefStm"))
            return std::unexpected(unsupported("incremental updates / hybrid xref are not supported; save a fresh copy first"));
        const DictEntry* root = findEntry(trailerEntries_, "Root");
        if (!root) return std::unexpected(invalidDoc("trailer has no /Root"));

        std::uint64_t size = maxNum_ + 1;
        if (const DictEntry* s = findEntry(trailerEntries_, "Size"); s && s->valEnd == s->valBegin + 1) {
            std::uint64_t v = 0;
            if (parseU64(trailer_[s->valBegin].text, v)) size = std::max<std::uint64_t>(size, v);
        }
        for (const auto& obj : objects_) size = std::max<std::uint64_t>(size, std::uint64_t{obj.num} + 1);
        if (size >= 0xFFFFFFFFull) return std::unexpected(invalidDoc("too many objects"));
        size_ = static_cast<std::uint32_t>(size);

        // Document ID: keep a well-formed one, otherwise mint a fresh one.
        bool haveId = false;
        if (const DictEntry* id = findEntry(trailerEntries_, "ID")) {
            if (id->valEnd - id->valBegin == 4 && trailer_[id->valBegin].kind == T::ArrOpen &&
                (trailer_[id->valBegin + 1].kind == T::Hex || trailer_[id->valBegin + 1].kind == T::Literal) &&
                (trailer_[id->valBegin + 2].kind == T::Hex || trailer_[id->valBegin + 2].kind == T::Literal)) {
                id0_.assign(trailer_[id->valBegin + 1].text.begin(), trailer_[id->valBegin + 1].text.end());
                id1_.assign(trailer_[id->valBegin + 2].text.begin(), trailer_[id->valBegin + 2].text.end());
                haveId = true;
            }
        }
        if (!haveId) {
            auto rnd = crypto::makeDefaultProvider()->randomBytes(32);
            if (!rnd) {
                return std::unexpected(rnd.error().code == crypto::CryptoErrorCode::BackendUnavailable
                                           ? core::makeError(core::ErrorCode::NotAvailable, rnd.error().message, kSubsystem)
                                           : crypto::toCoreError(rnd.error()));
            }
            id0_.assign(rnd->begin(), rnd->begin() + 16);
            id1_.assign(rnd->begin() + 16, rnd->end());
        }
        return core::ok();
    }

    // ---- objects ----
    std::optional<std::int64_t> resolveLength(std::uint32_t num) {
        auto it = offsetByNum_.find(num);
        if (it == offsetByNum_.end()) return std::nullopt;
        Lexer lx(d_, static_cast<std::size_t>(it->second));
        Tok toks[4];
        for (auto& t : toks) {
            auto n = lx.next();
            if (!n || !*n) return std::nullopt;
            t = std::move(**n);
        }
        std::uint64_t v = 0;
        if (!isKeyword(toks[2], "obj") || !parseU64(toks[3].text, v) || v > kMaxFileSize) return std::nullopt;
        return static_cast<std::int64_t>(v);
    }

    core::Status writeObject(const XrefObject& obj, Out& out) {
        Lexer lx(d_, static_cast<std::size_t>(obj.offset));
        std::vector<Tok> head;
        for (int i = 0; i < 3; ++i) {
            auto n = lx.next();
            if (!n) return std::unexpected(n.error());
            if (!*n) return std::unexpected(invalidDoc("truncated object header"));
            head.push_back(std::move(**n));
        }
        std::uint64_t num = 0;
        std::uint64_t gen = 0;
        if (!parseU64(head[0].text, num) || !parseU64(head[1].text, gen) || !isKeyword(head[2], "obj") || num != obj.num || gen != obj.gen)
            return std::unexpected(invalidDoc("xref entry does not point at its object"));

        std::vector<Tok> toks;
        bool isStream = false;
        std::size_t endPos = 0;
        int depth = 0;
        for (;;) {
            auto n = lx.next();
            if (!n) return std::unexpected(n.error());
            if (!*n) return std::unexpected(invalidDoc("object is not terminated by endobj"));
            Tok& t = **n;
            if (depth == 0 && isKeyword(t, "endobj")) {
                endPos = t.end;
                break;
            }
            if (depth == 0 && isKeyword(t, "stream")) {
                isStream = true;
                endPos = t.end;
                break;
            }
            if (t.kind == T::ArrOpen || t.kind == T::DictOpen) {
                if (++depth > kMaxDepth) return std::unexpected(invalidDoc("object nesting too deep"));
            } else if (t.kind == T::ArrClose || t.kind == T::DictClose) {
                if (--depth < 0) return std::unexpected(invalidDoc("unbalanced brackets in object"));
            }
            toks.push_back(std::move(t));
            if (toks.size() > kMaxObjectTokens) return std::unexpected(invalidDoc("object too large"));
        }

        std::vector<Substitution> subs;
        bool skipStreamEncryption = false;
        std::vector<DictEntry> entries;
        const bool dictBody = !toks.empty() && toks.front().kind == T::DictOpen;
        if (dictBody) {
            auto e = topEntries(toks, 0);
            if (!e) return std::unexpected(e.error());
            entries = std::move(*e);
        }

        for (const Tok& t : toks) {
            if (t.kind == T::Name) {
                if (t.text == "ByteRange")
                    return std::unexpected(unsupported("the document contains a digital signature; encrypting it would invalidate the signature (encrypt first, then sign)"));
                if (t.text == "Crypt")
                    return std::unexpected(unsupported("streams with an explicit /Crypt filter are not supported"));
            }
        }
        if (const DictEntry* type = findEntry(entries, "Type"); type && type->valEnd == type->valBegin + 1) {
            const Tok& v = toks[type->valBegin];
            if (v.kind == T::Name && (v.text == "ObjStm" || v.text == "XRef"))
                return std::unexpected(unsupported("object streams / xref streams are not supported (expected PDFium's classic output)"));
            if (v.kind == T::Name && v.text == "Metadata" && !options_.encryptMetadata) skipStreamEncryption = true;
        }

        for (const Tok& t : toks) {
            if (t.kind != T::Literal && t.kind != T::Hex) continue;
            auto enc = encryptObjectData(setup_, obj.num, obj.gen, ByteView(reinterpret_cast<const std::uint8_t*>(t.text.data()), t.text.size()));
            if (!enc) return std::unexpected(enc.error());
            subs.push_back({t.begin, t.end, hex(*enc)});
        }

        std::string dataPrefix;
        Bytes streamBytes;
        if (isStream) {
            if (!dictBody) return std::unexpected(invalidDoc("stream object without a dictionary"));
            const DictEntry* len = findEntry(entries, "Length");
            if (!len) return std::unexpected(invalidDoc("stream without /Length"));
            std::int64_t length = -1;
            std::uint64_t v = 0;
            if (len->valEnd == len->valBegin + 1 && parseU64(toks[len->valBegin].text, v)) {
                length = static_cast<std::int64_t>(v);
            } else if (len->valEnd == len->valBegin + 3 && parseU64(toks[len->valBegin].text, v) && v <= 0xFFFFFFFFull) {
                if (auto r = resolveLength(static_cast<std::uint32_t>(v))) length = *r;
            }
            if (length < 0) return std::unexpected(invalidDoc("cannot determine the stream /Length"));

            std::size_t dataBegin = endPos;
            if (dataBegin + 1 < d_.size() && d_[dataBegin] == '\r' && d_[dataBegin + 1] == '\n') dataBegin += 2;
            else if (dataBegin < d_.size() && d_[dataBegin] == '\n') dataBegin += 1;
            else return std::unexpected(invalidDoc("stream keyword is not followed by an end-of-line"));
            if (static_cast<std::uint64_t>(length) > d_.size() - dataBegin) return std::unexpected(invalidDoc("stream /Length exceeds the file"));
            const std::size_t dataEnd = dataBegin + static_cast<std::size_t>(length);
            Lexer after(d_, dataEnd);
            auto es = after.next();
            if (!es) return std::unexpected(es.error());
            if (!*es || !isKeyword(**es, "endstream")) return std::unexpected(invalidDoc("stream /Length does not match the stream data"));
            auto eo = after.next();
            if (!eo) return std::unexpected(eo.error());
            if (!*eo || !isKeyword(**eo, "endobj")) return std::unexpected(invalidDoc("endstream is not followed by endobj"));

            const ByteView plain(d_.data() + dataBegin, static_cast<std::size_t>(length));
            if (skipStreamEncryption) {
                streamBytes.assign(plain.begin(), plain.end());
            } else {
                auto enc = encryptObjectData(setup_, obj.num, obj.gen, plain);
                if (!enc) return std::unexpected(enc.error());
                streamBytes = std::move(*enc);
            }
            // A direct /Length replaces a direct or indirect one.
            subs.push_back({toks[len->valBegin].begin, toks[len->valEnd - 1].end, std::to_string(streamBytes.size())});
        }

        std::sort(subs.begin(), subs.end(), [](const Substitution& a, const Substitution& b) { return a.begin < b.begin; });
        std::size_t cursor = static_cast<std::size_t>(obj.offset);
        for (const auto& s : subs) {
            out.put(d_.data() + cursor, s.begin - cursor);
            out.put(s.text);
            cursor = s.end;
        }
        out.put(d_.data() + cursor, endPos - cursor);
        if (isStream) {
            out.put("\n");
            out.put(streamBytes);
            out.put("\nendstream\nendobj\n");
        } else {
            out.put("\n");
        }
        return core::ok();
    }

    // ---- xref + trailer ----
    void writeXrefAndTrailer(Out& out, const std::map<std::uint32_t, std::uint64_t>& offsets, std::uint32_t encryptNum) {
        const std::uint32_t newSize = encryptNum + 1;
        std::vector<std::uint32_t> freeNums;
        for (std::uint32_t n = 1; n < newSize; ++n)
            if (!offsets.count(n)) freeNums.push_back(n);

        const std::size_t xrefOffset = out.bytes.size();
        out.put("xref\n0 " + std::to_string(newSize) + "\n");
        char line[32];
        std::size_t freeIdx = 0;
        for (std::uint32_t n = 0; n < newSize; ++n) {
            if (n == 0) {
                std::snprintf(line, sizeof line, "%010u 65535 f \n", freeNums.empty() ? 0u : freeNums[0]);
            } else if (auto it = offsets.find(n); it != offsets.end()) {
                std::snprintf(line, sizeof line, "%010llu 00000 n \n", static_cast<unsigned long long>(it->second));
            } else {
                ++freeIdx;
                std::snprintf(line, sizeof line, "%010u 00001 f \n", freeIdx < freeNums.size() ? freeNums[freeIdx] : 0u);
            }
            out.put(std::string_view(line, 20));
        }

        out.put("trailer\n<<");
        for (const auto& e : trailerEntries_) {
            if (e.key == "Size" || e.key == "ID" || e.key == "Encrypt") continue;
            out.put(" ");
            out.put(d_.data() + trailer_[e.keyTok].begin, trailer_[e.valEnd - 1].end - trailer_[e.keyTok].begin);
        }
        out.put(" /Size " + std::to_string(newSize) + " /Encrypt " + std::to_string(encryptNum) + " 0 R /ID [" + hex(id0_) +
                hex(id1_) + "] >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n");
    }

    ByteView d_;
    const EncryptOptions& options_;
    int major_ = 1;
    int minor_ = 7;
    std::vector<XrefObject> objects_; // in file order
    std::map<std::uint32_t, std::uint64_t> offsetByNum_;
    std::uint64_t maxNum_ = 0;
    std::size_t trailerLexer_ = 0;
    std::vector<Tok> trailer_;
    std::vector<DictEntry> trailerEntries_;
    std::uint32_t size_ = 0;
    Bytes id0_;
    Bytes id1_;
    EncryptionSetup setup_;
};

} // namespace

core::Result<std::vector<std::uint8_t>> encryptPdf(std::span<const std::uint8_t> pdf, const EncryptOptions& options) {
    if (options.userPassword.empty() && options.ownerPassword.empty() && options.permissions.allAllowed())
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                               "no password and no permission restriction: nothing to protect", kSubsystem));
    if (options.userPassword.size() > 4096 || options.ownerPassword.size() > 4096)
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "password too long", kSubsystem));
    if (!crypto::makeDefaultProvider()->isAvailable())
        return std::unexpected(core::makeError(core::ErrorCode::NotAvailable,
                                               "PDF encryption needs a crypto backend (built with RIVET_ENABLE_CRYPTO=OFF)", kSubsystem));
    Encryptor encryptor(pdf, options);
    return encryptor.run();
}

} // namespace rivet::pdf::encrypt
