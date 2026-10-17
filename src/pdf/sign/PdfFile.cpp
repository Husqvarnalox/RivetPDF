// SPDX-License-Identifier: MPL-2.0
#include "pdf/sign/PdfFile.hpp"

#include "pdf/sign/PdfInflate.hpp"

#include <algorithm>
#include <cstring>

namespace rivet::pdf::sign {

namespace {

std::unexpected<SignError> bad(const char* what) {
    return std::unexpected<SignError>(SignError{SignErrorCode::MalformedPdf, what, std::nullopt});
}

bool matchAt(ByteView data, std::uint64_t pos, std::string_view word) {
    if (pos > data.size() || data.size() - pos < word.size()) return false;
    return std::memcmp(data.data() + pos, word.data(), word.size()) == 0;
}

// Position of the last occurrence of `word` in [from, data.size()), or npos.
constexpr std::uint64_t kNpos = ~std::uint64_t{0};
std::uint64_t findLast(ByteView data, std::uint64_t from, std::string_view word) {
    if (data.size() < word.size()) return kNpos;
    for (std::uint64_t i = data.size() - word.size() + 1; i-- > from;)
        if (std::memcmp(data.data() + i, word.data(), word.size()) == 0) return i;
    return kNpos;
}

std::uint64_t findFirst(ByteView data, std::uint64_t from, std::uint64_t limit, std::string_view word) {
    const std::uint64_t end = std::min<std::uint64_t>(data.size(), limit);
    if (from >= end || end - from < word.size()) return kNpos;
    const auto* hit = std::search(data.data() + from, data.data() + end, word.begin(), word.end());
    return hit == data.data() + end ? kNpos : static_cast<std::uint64_t>(hit - data.data());
}

// End offset of the revision's "%%EOF" following a trailer / xref stream.
std::uint64_t findEofEnd(ByteView data, std::uint64_t after) {
    const std::uint64_t sx = findFirst(data, after, after + 1024, "startxref");
    if (sx == kNpos) return 0;
    const std::uint64_t eof = findFirst(data, sx, sx + 128, "%%EOF");
    return eof == kNpos ? 0 : eof + 5;
}

std::optional<std::uint64_t> readUnsigned(ByteView data, std::uint64_t& pos, int maxDigits) {
    while (pos < data.size() && isPdfWhitespace(data[pos])) ++pos;
    std::uint64_t v = 0;
    int n = 0;
    while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') {
        if (++n > maxDigits) return std::nullopt;
        v = v * 10 + (data[pos++] - '0');
    }
    if (n == 0) return std::nullopt;
    return v;
}

} // namespace

std::optional<std::int64_t> dictInt(const PdfDict& d, std::string_view key) {
    const PdfObject* o = d.find(key);
    if (o && o->isInt()) return o->integer;
    return std::nullopt;
}

std::string textStringToUtf8(std::string_view s) {
    std::string out;
    const auto putCp = [&](unsigned cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };
    if (s.size() >= 2 && static_cast<std::uint8_t>(s[0]) == 0xFE && static_cast<std::uint8_t>(s[1]) == 0xFF) {
        for (std::size_t i = 2; i + 1 < s.size(); i += 2) {
            unsigned u = (static_cast<std::uint8_t>(s[i]) << 8) | static_cast<std::uint8_t>(s[i + 1]);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < s.size()) {
                const unsigned lo = (static_cast<std::uint8_t>(s[i + 2]) << 8) | static_cast<std::uint8_t>(s[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            putCp(u >= 0xD800 && u <= 0xDFFF ? 0xFFFD : u);
        }
        return out;
    }
    if (s.size() >= 3 && static_cast<std::uint8_t>(s[0]) == 0xEF && static_cast<std::uint8_t>(s[1]) == 0xBB &&
        static_cast<std::uint8_t>(s[2]) == 0xBF)
        return std::string(s.substr(3));
    for (const char ch : s) putCp(static_cast<std::uint8_t>(ch));
    return out;
}

// ---- open / xref chain ------------------------------------------------------------------

SignResult<PdfFile> PdfFile::open(ByteView data) {
    PdfFile f;
    f.data_ = data;
    if (data.size() < 16) return bad("file too small to be a PDF");
    const std::uint64_t header = findFirst(data, 0, 1024, "%PDF-");
    if (header == kNpos) return bad("missing %PDF- header");
    if (header != 0) return fail(SignErrorCode::UnsupportedStructure, "data before the %PDF- header");
    if (data.size() >= 8 && data[5] >= '0' && data[5] <= '9' && data[6] == '.' && data[7] >= '0' && data[7] <= '9')
        f.headerVersion_ = std::string(reinterpret_cast<const char*>(data.data()) + 5, 3);

    const std::uint64_t windowStart = data.size() > 4096 ? data.size() - 4096 : 0;
    const std::uint64_t sx = findLast(data, windowStart, "startxref");
    if (sx == kNpos) return bad("startxref not found");
    std::uint64_t p = sx + 9;
    const auto start = readUnsigned(data, p, 18);
    if (!start || *start >= data.size()) return bad("invalid startxref offset");
    f.lastStartxref_ = *start;

    std::set<std::uint64_t> visited;
    std::uint64_t next = *start;
    std::size_t totalEntries = 0;
    for (;;) {
        if (!visited.insert(next).second) return bad("cross-reference loop");
        if (f.sections_.size() >= kMaxXrefSections)
            return fail(SignErrorCode::LimitExceeded, "too many cross-reference sections");
        auto section = f.parseSection(next);
        if (!section) return std::unexpected(section.error());
        totalEntries += section->entries.size();
        if (totalEntries > kMaxXrefEntries) return fail(SignErrorCode::LimitExceeded, "too many cross-reference entries");
        const auto prev = dictInt(section->trailer, "Prev");
        f.sections_.push_back(std::move(*section));
        if (!prev || *prev <= 0) break;
        if (static_cast<std::uint64_t>(*prev) >= data.size()) return bad("invalid /Prev offset");
        next = static_cast<std::uint64_t>(*prev);
    }
    return f;
}

SignResult<XrefSection> PdfFile::parseSection(std::uint64_t offset) {
    XrefSection s;
    s.offset = offset;
    PdfParser p(data_, offset);
    p.skipWhitespaceAndComments();
    if (matchAt(data_, p.position(), "xref")) {
        if (auto r = parseTable(s, p.position()); !r) return std::unexpected(r.error());
    } else {
        if (auto r = parseStreamSection(s, offset); !r) return std::unexpected(r.error());
    }
    return s;
}

SignResult<void> PdfFile::parseTable(XrefSection& s, std::uint64_t offset) {
    PdfParser p(data_, offset);
    if (p.readKeyword() != "xref") return bad("expected xref");
    std::uint64_t pos = p.position();
    for (;;) {
        while (pos < data_.size() && isPdfWhitespace(data_[pos])) ++pos;
        if (matchAt(data_, pos, "trailer")) {
            pos += 7;
            break;
        }
        const auto first = readUnsigned(data_, pos, 10);
        const auto count = readUnsigned(data_, pos, 10);
        if (!first || !count) return bad("malformed xref subsection header");
        if (*count > (data_.size() - pos) / 18) return bad("xref subsection larger than the file");
        if (*first + *count > 0x7FFFFFFFull + 1) return bad("xref object number out of range");
        for (std::uint64_t i = 0; i < *count; ++i) {
            const auto off = readUnsigned(data_, pos, 19);
            const auto gen = readUnsigned(data_, pos, 6);
            if (!off || !gen) return bad("malformed xref entry");
            while (pos < data_.size() && isPdfWhitespace(data_[pos])) ++pos;
            if (pos >= data_.size() || (data_[pos] != 'n' && data_[pos] != 'f')) return bad("malformed xref entry type");
            XrefEntry e;
            if (data_[pos] == 'n') {
                e.type = XrefEntry::Type::InUse;
                e.a = *off;
                e.b = static_cast<std::uint32_t>(*gen & 0xFFFF);
            }
            ++pos;
            s.entries.emplace(static_cast<std::uint32_t>(*first + i), e);
        }
    }
    PdfParser tp(data_, pos);
    auto trailer = tp.parseObject();
    if (!trailer) return std::unexpected(trailer.error());
    if (!trailer->isDict()) return bad("trailer is not a dictionary");
    s.trailer = *trailer->dict;
    s.eofEnd = findEofEnd(data_, tp.position());

    if (const auto stm = dictInt(s.trailer, "XRefStm")) {
        if (*stm > 0 && static_cast<std::uint64_t>(*stm) < data_.size()) {
            XrefSection stream;
            stream.offset = static_cast<std::uint64_t>(*stm);
            if (auto r = parseStreamSection(stream, stream.offset); r) {
                s.hybrid = true;
                mergeXrefStream(s, stream);
            } else {
                return std::unexpected(r.error());
            }
        }
    }
    return {};
}

void PdfFile::mergeXrefStream(XrefSection& into, const XrefSection& stream) {
    // Hybrid files mark objects that live in the /XRefStm as free in the
    // table (old readers skip them): the stream supplies those, but never
    // replaces an in-use table entry.
    for (const auto& [num, entry] : stream.entries) {
        auto [it, inserted] = into.entries.emplace(num, entry);
        if (!inserted && it->second.type == XrefEntry::Type::Free) it->second = entry;
    }
}

SignResult<void> PdfFile::parseStreamSection(XrefSection& s, std::uint64_t offset) {
    auto io = parseIndirectObjectAt(data_, offset, nullptr);
    if (!io) return std::unexpected(io.error());
    if (!io->isStream || !io->value.isDict() || !io->value.dict->isType("XRef")) return bad("not a cross-reference stream");
    const PdfDict& d = *io->value.dict;
    s.isStream = true;
    s.streamObjNum = io->id.num;
    s.trailer = d;
    s.eofEnd = findEofEnd(data_, io->end);

    const PdfObject* w = d.find("W");
    if (!w || !w->isArray() || w->array->size() != 3) return bad("bad /W in xref stream");
    std::size_t widths[3];
    for (int i = 0; i < 3; ++i) {
        const PdfObject& v = (*w->array)[static_cast<std::size_t>(i)];
        if (!v.isInt() || v.integer < 0 || v.integer > 8) return bad("bad /W width");
        widths[i] = static_cast<std::size_t>(v.integer);
    }
    const std::size_t rowWidth = widths[0] + widths[1] + widths[2];
    if (rowWidth == 0) return bad("empty xref rows");
    const auto size = dictInt(d, "Size");
    if (!size || *size < 0 || *size > 0x7FFFFFFF) return bad("bad /Size in xref stream");

    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::uint64_t total = 0;
    if (const PdfObject* idx = d.find("Index")) {
        if (!idx->isArray() || idx->array->size() % 2 != 0) return bad("bad /Index");
        for (std::size_t i = 0; i < idx->array->size(); i += 2) {
            const PdfObject& a = (*idx->array)[i];
            const PdfObject& b = (*idx->array)[i + 1];
            if (!a.isInt() || !b.isInt() || a.integer < 0 || b.integer < 0 || a.integer > 0x7FFFFFFF ||
                b.integer > static_cast<std::int64_t>(kMaxXrefEntries))
                return bad("bad /Index entry");
            ranges.emplace_back(static_cast<std::uint64_t>(a.integer), static_cast<std::uint64_t>(b.integer));
            total += static_cast<std::uint64_t>(b.integer);
            if (total > kMaxXrefEntries) return fail(SignErrorCode::LimitExceeded, "too many xref stream entries");
        }
    } else {
        ranges.emplace_back(0, static_cast<std::uint64_t>(*size));
        total = static_cast<std::uint64_t>(*size);
        if (total > kMaxXrefEntries) return fail(SignErrorCode::LimitExceeded, "too many xref stream entries");
    }

    const std::size_t maxOut = static_cast<std::size_t>(total + 1) * (rowWidth + 1) + 64;
    auto decoded = decodeStream(*io, maxOut);
    if (!decoded) return std::unexpected(decoded.error());
    if (decoded->size() / rowWidth < total) return bad("xref stream shorter than its /Index");

    std::size_t pos = 0;
    const auto field = [&](std::size_t width) {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < width; ++i) v = (v << 8) | (*decoded)[pos++];
        return v;
    };
    for (const auto& [first, count] : ranges) {
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t type = widths[0] ? field(widths[0]) : 1;
            const std::uint64_t f2 = field(widths[1]);
            const std::uint64_t f3 = field(widths[2]);
            if (first + i > 0x7FFFFFFFull) return bad("xref stream object number out of range");
            XrefEntry e;
            if (type == 0) {
                e.type = XrefEntry::Type::Free;
            } else if (type == 1) {
                e.type = XrefEntry::Type::InUse;
                e.a = f2;
                e.b = static_cast<std::uint32_t>(f3 & 0xFFFF);
            } else if (type == 2) {
                if (f2 > 0x7FFFFFFFull) return bad("bad object stream number");
                e.type = XrefEntry::Type::Compressed;
                e.a = f2;
                e.b = static_cast<std::uint32_t>(f3 & 0xFFFFFFFF);
            } else {
                continue; // unknown type: treated as a null object
            }
            s.entries.emplace(static_cast<std::uint32_t>(first + i), e);
        }
    }
    return {};
}

// ---- stream decoding ---------------------------------------------------------------------------

SignResult<Bytes> PdfFile::decodeStream(const IndirectObject& stream, std::size_t maxOutput) {
    if (!stream.isStream || !stream.value.isDict()) return bad("not a stream");
    const PdfDict& d = *stream.value.dict;
    if (stream.streamBegin > data_.size() || stream.streamLength > data_.size() - stream.streamBegin)
        return bad("stream out of bounds");
    const ByteView raw(data_.data() + stream.streamBegin, static_cast<std::size_t>(stream.streamLength));

    bool flate = false;
    if (const PdfObject* f = d.find("Filter")) {
        const PdfObject* name = f;
        if (f->isArray()) {
            if (f->array->size() != 1) return fail(SignErrorCode::UnsupportedStructure, "filter chain not supported");
            name = &(*f->array)[0];
        }
        if (name->isName("FlateDecode") || name->isName("Fl")) flate = true;
        else return fail(SignErrorCode::UnsupportedStructure, "stream filter not supported");
    }
    if (!flate) {
        if (raw.size() > maxOutput) return fail(SignErrorCode::LimitExceeded, "stream too large");
        return Bytes(raw.begin(), raw.end());
    }
    auto inflated = inflateZlib(raw, maxOutput);
    if (!inflated) return inflated;

    const PdfDict* parms = nullptr;
    if (const PdfObject* dp = d.find("DecodeParms")) {
        const PdfObject* o = dp;
        if (dp->isArray() && dp->array->size() == 1) o = &(*dp->array)[0];
        if (o->isDict()) parms = o->dict.get();
        else if (!o->isNull()) return fail(SignErrorCode::UnsupportedStructure, "unsupported /DecodeParms");
    }
    if (parms) {
        const std::int64_t predictor = dictInt(*parms, "Predictor").value_or(1);
        if (predictor >= 10 && predictor <= 15) {
            if (dictInt(*parms, "BitsPerComponent").value_or(8) != 8)
                return fail(SignErrorCode::UnsupportedStructure, "predictor with non-8-bit components");
            const auto columns = dictInt(*parms, "Columns").value_or(1);
            const auto colors = dictInt(*parms, "Colors").value_or(1);
            if (columns < 1 || columns > (1 << 24) || colors < 1 || colors > 32) return bad("bad predictor parameters");
            return undoPngPredictor(*inflated, static_cast<unsigned>(columns), static_cast<unsigned>(colors));
        }
        if (predictor != 1) return fail(SignErrorCode::UnsupportedStructure, "unsupported predictor");
    }
    return inflated;
}

// ---- trailer / lookup -------------------------------------------------------------------------------

PdfDict PdfFile::mergedTrailer(std::size_t from) const {
    PdfDict out;
    static constexpr std::string_view kKeys[] = {"Root", "Info", "ID", "Encrypt", "Size"};
    for (std::size_t i = from; i < sections_.size(); ++i)
        for (const auto key : kKeys)
            if (const PdfObject* v = sections_[i].trailer.find(key); v && !out.find(key)) out.set(std::string(key), *v);
    return out;
}

bool PdfFile::isEncrypted() const {
    for (const XrefSection& s : sections_)
        if (s.trailer.find("Encrypt")) return true;
    return false;
}

std::uint64_t PdfFile::nextObjectNumber(std::size_t from) const {
    std::uint64_t next = 1;
    for (std::size_t i = from; i < sections_.size(); ++i) {
        if (!sections_[i].entries.empty()) next = std::max<std::uint64_t>(next, sections_[i].entries.rbegin()->first + 1ull);
        if (const auto size = dictInt(sections_[i].trailer, "Size"); size && *size > 0)
            next = std::max<std::uint64_t>(next, static_cast<std::uint64_t>(*size));
    }
    return next;
}

std::optional<XrefEntry> PdfFile::lookup(std::uint32_t num, std::size_t from) const {
    for (std::size_t i = from; i < sections_.size(); ++i) {
        const auto it = sections_[i].entries.find(num);
        if (it != sections_[i].entries.end()) return it->second;
    }
    return std::nullopt;
}

SignResult<IndirectObject> PdfFile::loadIndirect(std::uint32_t num, std::size_t from) {
    const auto entry = lookup(num, from);
    if (!entry || entry->type != XrefEntry::Type::InUse) return bad("object is not an in-use, uncompressed object");
    const LengthResolver resolver = [this, from](PdfRef r) -> std::optional<std::int64_t> {
        if (resolveDepth_ > 8) return std::nullopt;
        ++resolveDepth_;
        auto v = loadObject(r.num, from);
        --resolveDepth_;
        if (v && *v && (*v)->isInt()) return (*v)->integer;
        return std::nullopt;
    };
    auto io = parseIndirectObjectAt(data_, entry->a, &resolver);
    if (!io) return io;
    if (io->id.num != num) return bad("object number does not match the cross-reference entry");
    return io;
}

SignResult<std::shared_ptr<PdfFile::ObjectStream>> PdfFile::loadObjectStream(std::uint32_t stmNum, std::size_t from) {
    const auto entry = lookup(stmNum, from);
    if (!entry || entry->type != XrefEntry::Type::InUse) return bad("object stream is not an in-use object");
    if (const auto it = objStreamCache_.find(entry->a); it != objStreamCache_.end()) return it->second;
    auto io = loadIndirect(stmNum, from);
    if (!io) return std::unexpected(io.error());
    if (!io->isStream || !io->value.isDict() || !io->value.dict->isType("ObjStm")) return bad("not an object stream");
    const PdfDict& d = *io->value.dict;
    const auto n = dictInt(d, "N");
    const auto first = dictInt(d, "First");
    if (!n || !first || *n < 0 || *n > (1 << 20) || *first < 0) return bad("bad object stream parameters");
    auto decoded = decodeStream(*io, kMaxObjectStreamBytes);
    if (!decoded) return std::unexpected(decoded.error());
    auto os = std::make_shared<ObjectStream>();
    os->data = std::move(*decoded);
    os->first = static_cast<std::uint64_t>(*first);
    if (os->first > os->data.size()) return bad("object stream /First out of range");
    PdfParser p(os->data, 0);
    for (std::int64_t i = 0; i < *n; ++i) {
        auto num = p.parseObject();
        auto off = num ? p.parseObject() : num;
        if (!num || !off || !num->isInt() || !off->isInt() || num->integer < 0 || num->integer > 0x7FFFFFFF ||
            off->integer < 0)
            return bad("bad object stream header");
        os->index.emplace_back(static_cast<std::uint32_t>(num->integer), static_cast<std::uint64_t>(off->integer));
    }
    objStreamCache_[entry->a] = os;
    return os;
}

SignResult<std::optional<PdfObject>> PdfFile::loadObject(std::uint32_t num, std::size_t from) {
    const auto entry = lookup(num, from);
    if (!entry || entry->type == XrefEntry::Type::Free) return std::optional<PdfObject>{};
    if (entry->type == XrefEntry::Type::InUse) {
        auto io = loadIndirect(num, from);
        if (!io) return std::unexpected(io.error());
        return std::optional<PdfObject>(std::move(io->value));
    }
    auto stm = loadObjectStream(static_cast<std::uint32_t>(entry->a), from);
    if (!stm) return std::unexpected(stm.error());
    const ObjectStream& os = **stm;
    std::size_t index = entry->b;
    if (index >= os.index.size() || os.index[index].first != num) {
        const auto it = std::find_if(os.index.begin(), os.index.end(), [&](const auto& e) { return e.first == num; });
        if (it == os.index.end()) return bad("object not found in its object stream");
        index = static_cast<std::size_t>(it - os.index.begin());
    }
    const std::uint64_t off = os.index[index].second;
    if (off > os.data.size() || os.first > os.data.size() - off) return bad("object offset outside its object stream");
    PdfParser p(os.data, os.first + off);
    auto value = p.parseObject();
    if (!value) return std::unexpected(value.error());
    return std::optional<PdfObject>(std::move(*value));
}

PdfObject PdfFile::resolve(const PdfObject& object, std::size_t from) {
    PdfObject cur = object;
    for (int i = 0; i < 32 && cur.isRef(); ++i) {
        auto loaded = loadObject(cur.ref.num, from);
        if (!loaded || !*loaded) return PdfObject::null();
        cur = std::move(**loaded);
    }
    return cur.isRef() ? PdfObject::null() : cur;
}

SignResult<std::pair<PdfRef, PdfDict>> PdfFile::catalog(std::size_t from) {
    const PdfDict trailer = mergedTrailer(from);
    const PdfObject* root = trailer.find("Root");
    if (!root || !root->isRef()) return bad("trailer has no /Root reference");
    auto obj = loadObject(root->ref.num, from);
    if (!obj) return std::unexpected(obj.error());
    if (!*obj || !(*obj)->isDict()) return bad("catalog is not a dictionary");
    return std::pair<PdfRef, PdfDict>(root->ref, *(*obj)->dict);
}

SignResult<PageRef> PdfFile::findPage(std::size_t index, std::size_t from) {
    auto cat = catalog(from);
    if (!cat) return std::unexpected(cat.error());
    const PdfObject* pagesObj = cat->second.find("Pages");
    if (!pagesObj || !pagesObj->isRef()) return bad("catalog has no /Pages reference");

    struct Node {
        PdfRef ref;
        int depth;
    };
    std::vector<Node> stack{{pagesObj->ref, 0}};
    std::set<std::uint32_t> visited;
    std::size_t leaves = 0;
    std::size_t visitedCount = 0;
    while (!stack.empty()) {
        const Node node = stack.back();
        stack.pop_back();
        if (node.depth > 64) return fail(SignErrorCode::LimitExceeded, "page tree too deep");
        if (!visited.insert(node.ref.num).second) return bad("cycle in the page tree");
        if (++visitedCount > kMaxFieldNodes * 5) return fail(SignErrorCode::LimitExceeded, "page tree too large");
        auto obj = loadObject(node.ref.num, from);
        if (!obj) return std::unexpected(obj.error());
        if (!*obj || !(*obj)->isDict()) return bad("page tree node is not a dictionary");
        const PdfDict& d = *(*obj)->dict;
        const PdfObject kids = d.find("Kids") ? resolve(*d.find("Kids"), from) : PdfObject::null();
        if (kids.isArray() && !d.isType("Page")) {
            for (auto it = kids.array->rbegin(); it != kids.array->rend(); ++it)
                if (it->isRef()) stack.push_back({it->ref, node.depth + 1});
            continue;
        }
        if (leaves++ == index) return PageRef{node.ref, d};
    }
    return fail(SignErrorCode::InvalidArgument, "page index out of range");
}

SignResult<std::vector<FieldInfo>> PdfFile::fields(std::size_t from) {
    std::vector<FieldInfo> result;
    auto cat = catalog(from);
    if (!cat) return std::unexpected(cat.error());
    const PdfObject* af = cat->second.find("AcroForm");
    if (!af) return result;
    const PdfObject acro = resolve(*af, from);
    if (!acro.isDict()) return result;
    const PdfObject* fieldsObj = acro.dict->find("Fields");
    if (!fieldsObj) return result;
    const PdfObject top = resolve(*fieldsObj, from);
    if (!top.isArray()) return result;

    struct Item {
        PdfObject node;
        std::string prefix;
        std::string ft;
        std::uint32_t ff;
        int depth;
    };
    std::vector<Item> stack;
    for (auto it = top.array->rbegin(); it != top.array->rend(); ++it) stack.push_back({*it, "", "", 0, 0});
    std::set<std::uint32_t> visited;
    std::size_t nodes = 0;
    while (!stack.empty()) {
        Item item = std::move(stack.back());
        stack.pop_back();
        if (item.depth > 64) return fail(SignErrorCode::LimitExceeded, "field tree too deep");
        if (++nodes > kMaxFieldNodes) return fail(SignErrorCode::LimitExceeded, "too many form fields");
        std::optional<PdfRef> ref;
        PdfObject node = item.node;
        if (node.isRef()) {
            if (!visited.insert(node.ref.num).second) continue;
            ref = node.ref;
            auto loaded = loadObject(node.ref.num, from);
            if (!loaded || !*loaded) continue;
            node = std::move(**loaded);
        }
        if (!node.isDict()) continue;
        const PdfDict& d = *node.dict;

        std::string full = item.prefix;
        if (const PdfObject* t = d.find("T"); t && t->isString()) {
            const std::string part = textStringToUtf8(t->text);
            full = full.empty() ? part : full + "." + part;
        }
        std::string ft = item.ft;
        if (const PdfObject* f = d.find("FT"); f && f->isName()) ft = f->text;
        std::uint32_t ff = item.ff;
        if (const auto v = dictInt(d, "Ff"); v && *v >= 0) ff = static_cast<std::uint32_t>(*v & 0xFFFFFFFF);

        PdfObject kids = d.find("Kids") ? resolve(*d.find("Kids"), from) : PdfObject::null();
        std::vector<PdfObject> fieldKids;
        std::vector<PdfRef> widgetKids;
        if (kids.isArray()) {
            for (const PdfObject& k : *kids.array) {
                if (!k.isRef()) continue;
                auto kd = loadObject(k.ref.num, from);
                if (!kd || !*kd || !(*kd)->isDict()) continue;
                if ((*kd)->dict->find("T")) fieldKids.push_back(k);
                else widgetKids.push_back(k.ref);
            }
        }
        if (!fieldKids.empty()) {
            for (auto it = fieldKids.rbegin(); it != fieldKids.rend(); ++it)
                stack.push_back({*it, full, ft, ff, item.depth + 1});
            continue;
        }
        FieldInfo info;
        info.ref = ref;
        info.dict = d;
        info.fullName = std::move(full);
        info.fieldType = std::move(ft);
        info.widgets = std::move(widgetKids);
        info.flags = ff;
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace rivet::pdf::sign
