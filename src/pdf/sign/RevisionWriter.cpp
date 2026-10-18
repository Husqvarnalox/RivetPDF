// SPDX-License-Identifier: MPL-2.0
#include "pdf/sign/RevisionWriter.hpp"

#include <cstdio>
#include <map>

namespace rivet::pdf::sign {

NewObject plainObject(std::uint32_t num, std::uint32_t gen, const PdfObject& value) {
    NewObject o;
    o.num = num;
    o.gen = gen;
    o.head = serialize(value);
    return o;
}

NewObject streamObject(std::uint32_t num, std::string data, PdfDict extra) {
    NewObject o;
    o.num = num;
    o.stream = true;
    extra.set("Length", PdfObject::makeInt(static_cast<std::int64_t>(data.size())));
    o.head = serialize(PdfObject::makeDict(std::move(extra)));
    o.streamData = std::move(data);
    return o;
}

std::uint32_t generationOf(PdfFile& f, std::uint32_t num) {
    const auto e = f.lookup(num);
    return e && e->type == XrefEntry::Type::InUse ? e->b : 0;
}

SignResult<RevisionWriter> RevisionWriter::begin(PdfFile& f) {
    if (f.isEncrypted())
        return fail(SignErrorCode::EncryptedDocument, "the document is encrypted; the file is not modified (ADR-0037)");
    RevisionWriter w;
    w.next_ = f.nextObjectNumber();
    w.prev_ = f.lastStartxref();
    w.streamXref_ = f.section(0).isStream;
    const PdfDict trailer = f.mergedTrailer();
    const PdfObject* root = trailer.find("Root");
    if (!root || !root->isRef()) return fail(SignErrorCode::MalformedPdf, "trailer has no /Root");
    w.trailerKeys_.set("Root", *root);
    for (const char* key : {"Info", "ID"})
        if (const PdfObject* v = trailer.find(key)) w.trailerKeys_.set(key, *v);
    return w;
}

SignResult<std::uint32_t> RevisionWriter::allocate() {
    if (next_ >= 0x7FFFFFF0ull) return fail(SignErrorCode::LimitExceeded, "object numbers exhausted");
    return static_cast<std::uint32_t>(next_++);
}

void RevisionWriter::add(NewObject object) { objects_.push_back(std::move(object)); }

SignOk RevisionWriter::appendTo(Bytes& pdf) {
    if (pdf.size() > kMaxSignableFileSize) return fail(SignErrorCode::LimitExceeded, "file too large (limit 2 GiB)");
    if (streamXref_ && xrefNum_ == 0) {
        auto n = allocate();
        if (!n) return std::unexpected(n.error());
        xrefNum_ = *n;
    }
    const std::uint64_t baseSize = pdf.size();
    std::string rev;
    if (pdf.empty() || (pdf.back() != '\n' && pdf.back() != '\r')) rev.push_back('\n');
    std::map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>> offsets;
    for (const NewObject& o : objects_) {
        offsets[o.num] = {baseSize + rev.size(), o.gen};
        rev += std::to_string(o.num) + " " + std::to_string(o.gen) + " obj\n";
        rev += o.head;
        if (o.stream) {
            rev += "\nstream\n";
            rev += o.streamData;
            rev += "\nendstream";
        }
        rev += "\nendobj\n";
    }
    const std::uint64_t xrefOffset = baseSize + rev.size();
    const std::uint64_t sizeKey = next_;
    std::vector<std::pair<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>>> all;
    if (xrefNum_ != 0) offsets[xrefNum_] = {xrefOffset, 0};
    all.assign(offsets.begin(), offsets.end());
    if (xrefNum_ == 0) {
        rev += "xref\n";
        std::size_t i = 0;
        while (i < all.size()) {
            std::size_t j = i + 1;
            while (j < all.size() && all[j].first == all[j - 1].first + 1) ++j;
            rev += std::to_string(all[i].first) + " " + std::to_string(j - i) + "\n";
            for (std::size_t k = i; k < j; ++k) {
                char line[32];
                std::snprintf(line, sizeof line, "%010llu %05u n \n", static_cast<unsigned long long>(all[k].second.first),
                              all[k].second.second);
                rev += line;
            }
            i = j;
        }
        PdfDict tr = trailerKeys_;
        tr.set("Size", PdfObject::makeInt(static_cast<std::int64_t>(sizeKey)));
        tr.set("Prev", PdfObject::makeInt(static_cast<std::int64_t>(prev_)));
        rev += "trailer\n" + serialize(PdfObject::makeDict(tr)) + "\n";
    } else {
        std::string data;
        PdfArray index;
        std::size_t i = 0;
        while (i < all.size()) {
            std::size_t j = i + 1;
            while (j < all.size() && all[j].first == all[j - 1].first + 1) ++j;
            index.push_back(PdfObject::makeInt(all[i].first));
            index.push_back(PdfObject::makeInt(static_cast<std::int64_t>(j - i)));
            for (std::size_t k = i; k < j; ++k) {
                const std::uint64_t off = all[k].second.first;
                data.push_back(1);
                for (int sh = 24; sh >= 0; sh -= 8) data.push_back(static_cast<char>((off >> sh) & 0xFF));
                data.push_back(static_cast<char>((all[k].second.second >> 8) & 0xFF));
                data.push_back(static_cast<char>(all[k].second.second & 0xFF));
            }
            i = j;
        }
        PdfDict xd = trailerKeys_;
        xd.set("Type", PdfObject::makeName("XRef"));
        xd.set("Size", PdfObject::makeInt(static_cast<std::int64_t>(sizeKey)));
        xd.set("Prev", PdfObject::makeInt(static_cast<std::int64_t>(prev_)));
        xd.set("W", PdfObject::makeArray({PdfObject::makeInt(1), PdfObject::makeInt(4), PdfObject::makeInt(2)}));
        xd.set("Index", PdfObject::makeArray(std::move(index)));
        xd.set("Length", PdfObject::makeInt(static_cast<std::int64_t>(data.size())));
        rev += std::to_string(xrefNum_) + " 0 obj\n" + serialize(PdfObject::makeDict(xd)) + "\nstream\n";
        rev += data;
        rev += "\nendstream\nendobj\n";
    }
    rev += "startxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
    if (baseSize + rev.size() > kMaxSignableFileSize)
        return fail(SignErrorCode::LimitExceeded, "the file would exceed 2 GiB (ByteRange limit)");
    pdf.insert(pdf.end(), rev.begin(), rev.end());
    return {};
}

} // namespace rivet::pdf::sign
